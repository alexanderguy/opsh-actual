#include "agent/event.h"
#include "compiler/compiler.h"
#include "exec/signal.h"
#include "foundation/util.h"
#include "lsp/lsp.h"
#include "parser/parser.h"
#include "vm/image_io.h"
#include "vm/vm.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = xmalloc((size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

/*
 * Appended payload trailer (8 bytes at end of executable):
 *   4 bytes: payload offset from start of file (u32 LE)
 *   4 bytes: trailer magic "OPSB"
 */
#define TRAILER_MAGIC "OPSB"
#define TRAILER_SIZE 8

/* Get the path to the current executable */
static char *get_self_exe(void)
{
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *buf = xmalloc(size);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
#else
    /* Linux: /proc/self/exe */
    char *buf = xmalloc(4096);
    ssize_t len = readlink("/proc/self/exe", buf, 4095);
    if (len < 0) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
#endif
}

/* Check if this executable has an appended .opsb payload.
 * Returns the loaded image, or NULL if no payload is present. */
static bytecode_image_t *load_appended_image(void)
{
    char *exe = get_self_exe();
    if (exe == NULL) {
        return NULL;
    }

    FILE *f = fopen(exe, "rb");
    free(exe);
    if (f == NULL) {
        return NULL;
    }

    /* Read trailer from end of file */
    if (fseek(f, -(long)TRAILER_SIZE, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    unsigned char trailer[TRAILER_SIZE];
    if (fread(trailer, 1, TRAILER_SIZE, f) != TRAILER_SIZE) {
        fclose(f);
        return NULL;
    }

    /* Check trailer magic */
    if (memcmp(trailer + 4, TRAILER_MAGIC, 4) != 0) {
        fclose(f);
        return NULL;
    }

    /* Read payload offset (u32 LE) */
    uint32_t offset = (uint32_t)trailer[0] | ((uint32_t)trailer[1] << 8) |
                      ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    bytecode_image_t *img = image_read_opsb(f);
    fclose(f);
    return img;
}

/* Copy a file. Returns 0 on success. */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }

    int err = ferror(in);
    fclose(in);
    fclose(out);
    return err ? -1 : 0;
}

static void usage(const char *progname)
{
    fprintf(stderr, "usage: %s [options] <script>\n", progname);
    fprintf(stderr, "       %s build <script.opsh> -o <output.opsb>\n", progname);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  --agent-stdio   emit JSON-RPC events to stderr\n");
}

/* Compile a .opsh script to a bytecode image */
static bytecode_image_t *compile_script(const char *path)
{
    char *source = read_file(path);
    if (source == NULL) {
        fprintf(stderr, "opsh: cannot read %s\n", path);
        return NULL;
    }

    parser_t p;
    parser_init(&p, source, path);
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        fprintf(stderr, "opsh: parse errors in %s\n", path);
        sh_list_free(ast);
        parser_destroy(&p);
        free(source);
        return NULL;
    }

    bytecode_image_t *img = compile(ast, path);
    sh_list_free(ast);
    parser_destroy(&p);
    free(source);

    return img;
}

/* Execute a bytecode image */
static int run_image(bytecode_image_t *img, const char *filename, int agent_stdio)
{
    vm_t vm;
    vm_init(&vm, img);

    event_sink_t *sink = NULL;
    if (agent_stdio) {
        int event_fd = dup(STDERR_FILENO);
        {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        sink = event_sink_stdio(event_fd);
        vm.event_sink = sink;
    }

    {
        event_t ev = {0};
        ev.type = EVENT_SCRIPT_START;
        ev.filename = filename;
        event_emit(vm.event_sink, &ev);
    }

    int status = vm_run(&vm);

    {
        event_t ev = {0};
        ev.type = EVENT_SCRIPT_END;
        ev.status = status;
        event_emit(vm.event_sink, &ev);
    }

    event_sink_free(sink);
    vm_destroy(&vm);

    return status;
}

int main(int argc, char *argv[])
{
    /* Check for appended bytecode payload (standalone binary mode) */
    {
        bytecode_image_t *img = load_appended_image();
        if (img != NULL) {
            signal_init();
            int agent_stdio = 0;
            int i;
            for (i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--agent-stdio") == 0) {
                    agent_stdio = 1;
                }
            }
            int status = run_image(img, argv[0], agent_stdio);
            image_free(img);
            return status;
        }
    }

    const char *script_path = NULL;
    const char *output_path = NULL;
    int agent_stdio = 0;
    int do_build = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "lsp") == 0 && i == 1) {
            return lsp_main();
        } else if (strcmp(argv[i], "build") == 0 && i == 1) {
            do_build = 1;
        } else if (strcmp(argv[i], "--agent-stdio") == 0) {
            agent_stdio = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "opsh: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (script_path == NULL) {
            script_path = argv[i];
        }
    }

    if (script_path == NULL) {
        fprintf(stderr, "opsh: no script specified\n");
        usage(argv[0]);
        return 1;
    }

    signal_init();

    /* opsh build: compile to .opsb or standalone binary */
    if (do_build) {
        /* Default output: strip extension from script path */
        char *default_output = NULL;
        if (output_path == NULL) {
            const char *dot = strrchr(script_path, '.');
            const char *slash = strrchr(script_path, '/');
            /* Only strip if the dot is after the last slash (part of the filename) */
            if (dot != NULL && (slash == NULL || dot > slash)) {
                size_t base_len = (size_t)(dot - script_path);
                default_output = xmalloc(base_len + 1);
                memcpy(default_output, script_path, base_len);
                default_output[base_len] = '\0';
            } else {
                size_t slen = strlen(script_path);
                default_output = xmalloc(slen + 5);
                memcpy(default_output, script_path, slen);
                memcpy(default_output + slen, ".out", 5);
            }
            output_path = default_output;
        }

        bytecode_image_t *img = compile_script(script_path);
        if (img == NULL) {
            return 2;
        }

        /* If output ends in .opsb, write raw bytecode; otherwise build standalone */
        size_t out_len = strlen(output_path);
        int standalone = !(out_len > 5 && strcmp(output_path + out_len - 5, ".opsb") == 0);

        if (standalone) {
            /* Copy the opsh binary, then append .opsb payload + trailer */
            char *exe = get_self_exe();
            if (exe == NULL) {
                fprintf(stderr, "opsh: cannot determine own executable path\n");
                image_free(img);
                return 1;
            }

            if (copy_file(exe, output_path) != 0) {
                fprintf(stderr, "opsh: cannot copy %s to %s\n", exe, output_path);
                free(exe);
                image_free(img);
                return 1;
            }
            free(exe);

            FILE *out = fopen(output_path, "ab");
            if (out == NULL) {
                fprintf(stderr, "opsh: cannot open %s for appending\n", output_path);
                image_free(img);
                return 1;
            }

            /* Record where the payload starts */
            fseek(out, 0, SEEK_END);
            long payload_offset = ftell(out);

            int result = image_write_opsb(img, out);
            if (result != 0) {
                fclose(out);
                image_free(img);
                fprintf(stderr, "opsh: write error\n");
                return 1;
            }

            /* Write trailer: payload offset (u32 LE) + magic "OPSB" */
            uint32_t off32 = (uint32_t)payload_offset;
            unsigned char trailer[TRAILER_SIZE];
            trailer[0] = off32 & 0xFF;
            trailer[1] = (off32 >> 8) & 0xFF;
            trailer[2] = (off32 >> 16) & 0xFF;
            trailer[3] = (off32 >> 24) & 0xFF;
            memcpy(trailer + 4, TRAILER_MAGIC, 4);
            fwrite(trailer, 1, TRAILER_SIZE, out);

            fclose(out);
            image_free(img);

            /* Make executable */
            chmod(output_path, 0755);
            return 0;
        }

        /* Raw .opsb output */
        FILE *out = fopen(output_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "opsh: cannot open %s for writing\n", output_path);
            image_free(img);
            return 1;
        }

        int result = image_write_opsb(img, out);
        fclose(out);
        image_free(img);

        if (result != 0) {
            fprintf(stderr, "opsh: write error\n");
            return 1;
        }
        return 0;
    }

    /* Check if input is a .opsb file */
    {
        size_t len = strlen(script_path);
        if (len > 5 && strcmp(script_path + len - 5, ".opsb") == 0) {
            /* Load pre-compiled bytecode */
            FILE *in = fopen(script_path, "rb");
            if (in == NULL) {
                fprintf(stderr, "opsh: cannot read %s\n", script_path);
                return 1;
            }
            bytecode_image_t *img = image_read_opsb(in);
            fclose(in);
            if (img == NULL) {
                fprintf(stderr, "opsh: failed to load %s\n", script_path);
                return 2;
            }
            int status = run_image(img, script_path, agent_stdio);
            image_free(img);
            return status;
        }
    }

    /* Compile and run .opsh script */
    bytecode_image_t *img = compile_script(script_path);
    if (img == NULL) {
        return 2;
    }

    int status = run_image(img, script_path, agent_stdio);
    image_free(img);

    return status;
}
