#include "serve/child.h"

#include "compiler/compiler.h"
#include "foundation/util.h"
#include "vm/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTROL_FD 3
#define MAX_CMD_SIZE (1024 * 1024) /* 1MB */

/* Read exactly n bytes, retrying on EINTR and short reads. */
static int read_exact(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, (char *)buf + total, n - total);
        if (r > 0) {
            total += (size_t)r;
        } else if (r == 0) {
            return -1; /* EOF */
        } else if (errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int child_loop_main(void)
{
    /* Ignore SIGPIPE so writes to a dead control pipe return EPIPE
     * instead of killing the process. */
    signal(SIGPIPE, SIG_IGN);

    /* Verify control fd is open */
    if (fcntl(CONTROL_FD, F_GETFD) < 0) {
        fprintf(stderr, "opsh: child-loop: control fd %d not open\n", CONTROL_FD);
        return 1;
    }

    /* The parent VM is a session context -- it holds environment and
     * function state between commands but is never executed directly.
     * Each command runs in a sub-VM via vm_exec_string. */
    bytecode_image_t *img = image_new();
    image_emit_u8(img, OP_HALT);

    vm_t vm;
    vm_init(&vm, img);

    for (;;) {
        /* Read 4-byte LE length */
        uint8_t len_buf[4];
        if (read_exact(STDIN_FILENO, len_buf, 4) != 0) {
            break; /* stdin closed */
        }
        uint32_t len = le32(len_buf);

        if (len > MAX_CMD_SIZE) {
            fprintf(stderr, "opsh: command too large (%u bytes)\n", len);
            break;
        }

        /* Read source */
        char *source = xmalloc(len + 1);
        if (read_exact(STDIN_FILENO, source, len) != 0) {
            free(source);
            break;
        }
        source[len] = '\0';

        /* Execute in persistent context */
        vm_exec_string(&vm, source, "<session>");
        free(source);

        /* Reset exit/halt flags so the session continues.
         * In a persistent session, exit means "end this command"
         * not "end the session." */
        vm.halted = false;
        vm.exit_requested = false;
        vm.return_requested = false;

        fflush(stdout);
        fflush(stderr);

        /* Signal completion with exit status on control fd */
        uint8_t status = (uint8_t)(vm.laststatus & 0xFF);
        ssize_t w = write(CONTROL_FD, &status, 1);
        if (w <= 0) {
            break; /* parent gone */
        }
    }

    int last = vm.laststatus;
    vm_destroy(&vm);
    image_free(img);
    return last;
}
