#ifndef OPSH_TESTS_POSIX_HELPERS_H
#define OPSH_TESTS_POSIX_HELPERS_H

#include "../tap.h"
#include "compiler/compiler.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"
#include "parser/parser.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Compile and run shell source, capture stdout.
 * Returns malloc'd string (caller frees). Never returns NULL.
 * On compile error, sets *status_out = 2 and returns "".
 */
TAP_UNUSED static char *run(const char *source, int *status_out)
{
    parser_t p;
    parser_init(&p, source, "test");
    sh_list_t *ast = parser_parse(&p);
    if (ast == NULL || parser_error_count(&p) > 0) {
        sh_list_free(ast);
        parser_destroy(&p);
        if (status_out)
            *status_out = 2;
        return xstrdup("");
    }

    bytecode_image_t *img = compile(ast, "test");
    sh_list_free(ast);
    parser_destroy(&p);
    if (img == NULL) {
        if (status_out)
            *status_out = 2;
        return xstrdup("");
    }

    int pipefd[2];
    pipe(pipefd);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    vm_t vm;
    vm_init(&vm, img);
    int status = vm_run(&vm);
    if (status_out)
        *status_out = status;

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    strbuf_t captured;
    strbuf_init(&captured);
    {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            strbuf_append_bytes(&captured, buf, (size_t)n);
        }
    }
    close(pipefd[0]);

    vm_destroy(&vm);
    image_free(img);
    return strbuf_detach(&captured);
}

/*
 * All-in-one: compile, run, check output and status.
 * Uses 3 TAP assertions: compiled, output matches, status matches.
 */
TAP_UNUSED static void run_ok(const char *source, const char *expected_out, int expected_status,
                              const char *desc)
{
    int status;
    char *out = run(source, &status);
    tap_ok(out != NULL, desc);
    tap_is_str(out, expected_out, desc);
    tap_is_int(status, expected_status, desc);
    free(out);
}

/*
 * Check status only, ignore output. Uses 2 TAP assertions.
 */
TAP_UNUSED static void run_status(const char *source, int expected_status, const char *desc)
{
    int status;
    char *out = run(source, &status);
    tap_ok(out != NULL, desc);
    tap_is_int(status, expected_status, desc);
    free(out);
}

/*
 * Ensure tmp/ directory exists for tests that need temp files.
 */
TAP_UNUSED static void ensure_tmp(void)
{
    mkdir("tmp", 0755);
}

#endif /* OPSH_TESTS_POSIX_HELPERS_H */
