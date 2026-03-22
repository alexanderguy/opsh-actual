#include "agent/event.h"
#include "compiler/compiler.h"
#include "exec/signal.h"
#include "foundation/util.h"
#include "parser/parser.h"
#include "vm/image_io.h"
#include "vm/vm.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    const char *script_path = NULL;
    const char *output_path = NULL;
    int agent_stdio = 0;
    int do_build = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "build") == 0 && i == 1) {
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

    /* opsh build: compile to .opsb */
    if (do_build) {
        if (output_path == NULL) {
            fprintf(stderr, "opsh: build requires -o <output>\n");
            return 1;
        }

        bytecode_image_t *img = compile_script(script_path);
        if (img == NULL) {
            return 2;
        }

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
