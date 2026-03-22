#include "agent/event.h"
#include "compiler/compiler.h"
#include "exec/signal.h"
#include "foundation/util.h"
#include "parser/parser.h"
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
    fprintf(stderr, "usage: %s [--agent-stdio] <script.opsh>\n", progname);
}

int main(int argc, char *argv[])
{
    const char *script_path = NULL;
    int agent_stdio = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent-stdio") == 0) {
            agent_stdio = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "opsh: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            script_path = argv[i];
        }
    }

    if (script_path == NULL) {
        fprintf(stderr, "opsh: no script specified\n");
        usage(argv[0]);
        return 1;
    }

    /* Initialize signal handling */
    signal_init();

    /* Read script */
    char *source = read_file(script_path);
    if (source == NULL) {
        fprintf(stderr, "opsh: cannot read %s\n", script_path);
        return 1;
    }

    /* Parse */
    parser_t p;
    parser_init(&p, source, script_path);
    sh_list_t *ast = parser_parse(&p);

    if (parser_error_count(&p) > 0) {
        fprintf(stderr, "opsh: parse errors in %s\n", script_path);
        sh_list_free(ast);
        parser_destroy(&p);
        free(source);
        return 2;
    }

    /* Compile */
    bytecode_image_t *img = compile(ast, script_path);
    sh_list_free(ast);
    parser_destroy(&p);
    free(source);

    if (img == NULL) {
        fprintf(stderr, "opsh: compilation failed for %s\n", script_path);
        return 2;
    }

    /* Execute */
    vm_t vm;
    vm_init(&vm, img);

    event_sink_t *sink = NULL;
    if (agent_stdio) {
        /* Use a dedicated FD for the event stream, separate from stderr.
         * Dup stderr to a high FD for the event stream, then redirect
         * stderr to /dev/null so VM diagnostics don't corrupt the stream. */
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

    /* Emit scriptStart */
    {
        event_t ev = {0};
        ev.type = EVENT_SCRIPT_START;
        ev.filename = script_path;
        event_emit(vm.event_sink, &ev);
    }

    int status = vm_run(&vm);

    /* Emit scriptEnd */
    {
        event_t ev = {0};
        ev.type = EVENT_SCRIPT_END;
        ev.status = status;
        event_emit(vm.event_sink, &ev);
    }

    event_sink_free(sink);
    vm_destroy(&vm);
    image_free(img);

    return status;
}
