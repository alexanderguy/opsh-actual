#include "helpers.h"

static void test_output_redirect(void)
{
    int status;
    char *out;
    char *contents;

    /* echo hello > file writes to file */
    out = run("echo hello > tmp/posix_redir.txt", &status);
    tap_is_int(status, 0, "output redir: exit status 0");
    tap_is_str(out, "", "output redir: nothing on stdout");
    free(out);

    contents = read_file("tmp/posix_redir.txt");
    tap_is_str(contents, "hello\n", "output redir: file contains output");
    free(contents);

    /* Truncation: second write replaces first */
    out = run("echo first > tmp/posix_redir.txt; echo second > tmp/posix_redir.txt", &status);
    tap_is_int(status, 0, "output redir truncate: exit status 0");
    free(out);

    contents = read_file("tmp/posix_redir.txt");
    tap_is_str(contents, "second\n", "output redir truncate: file has only second");
    free(contents);

    /* Nothing on stdout when redirected */
    out = run("echo gone > tmp/posix_redir.txt", &status);
    tap_is_str(out, "", "output redir: stdout is empty");
    free(out);

    unlink("tmp/posix_redir.txt");
}

static void test_append_redirect(void)
{
    int status;
    char *out;
    char *contents;

    out = run("echo a > tmp/posix_app.txt; echo b >> tmp/posix_app.txt", &status);
    tap_is_int(status, 0, "append redir: exit status 0");
    free(out);

    contents = read_file("tmp/posix_app.txt");
    tap_is_str(contents, "a\nb\n", "append redir: file has both lines");
    free(contents);

    /* Append a third line */
    out = run("echo c >> tmp/posix_app.txt", &status);
    tap_is_int(status, 0, "append redir: third append exit status 0");
    free(out);

    contents = read_file("tmp/posix_app.txt");
    tap_is_str(contents, "a\nb\nc\n", "append redir: file has three lines");
    free(contents);

    unlink("tmp/posix_app.txt");
}

static void test_heredoc_expansion(void)
{
    char *out;
    int status;

    /* Basic heredoc with variable expansion */
    out = run("X=world; cat <<EOF\nhello $X\nEOF\n", &status);
    tap_is_str(out, "hello world\n", "heredoc: variable expansion");
    tap_is_int(status, 0, "heredoc: expansion exit status 0");
    free(out);
}

static void test_heredoc_quoted(void)
{
    char *out;
    int status;

    /* Quoted delimiter suppresses expansion */
    out = run("X=world; cat <<'EOF'\nhello $X\nEOF\n", &status);
    tap_is_str(out, "hello $X\n", "heredoc: quoted delimiter suppresses expansion");
    tap_is_int(status, 0, "heredoc: quoted exit status 0");
    free(out);
}

static void test_heredoc_tab_strip(void)
{
    char *out;
    int status;

    /* <<- strips leading tabs */
    out = run("cat <<-EOF\n\thello\nEOF\n", &status);
    tap_is_str(out, "hello\n", "heredoc: <<- strips leading tabs");
    tap_is_int(status, 0, "heredoc: tab strip exit status 0");
    free(out);
}

static void test_fd_duplication(void)
{
    int status;
    char *out;
    char *contents;

    /* 2>&1 should not crash */
    out = run("echo hello 2>&1", &status);
    tap_is_int(status, 0, "fd dup: 2>&1 exit status 0");
    free(out);

    /* Redirect ordering: stdout to file, stderr to stdout (which is the file) */
    out = run("echo out > tmp/posix_order.txt 2>&1", &status);
    tap_is_int(status, 0, "fd dup ordering: exit status 0");
    free(out);

    contents = read_file("tmp/posix_order.txt");
    tap_is_str(contents, "out\n", "fd dup ordering: file has output");
    free(contents);

    unlink("tmp/posix_order.txt");
}

static void test_here_string(void)
{
    char *out;
    int status;

    out = run("cat <<< \"hello\"", &status);
    tap_is_str(out, "hello", "here-string: basic");
    tap_is_int(status, 0, "here-string: exit status 0");
    free(out);

    /* Here-string with variable expansion */
    out = run("X=world; cat <<< \"hello $X\"", &status);
    tap_is_str(out, "hello world", "here-string: variable expansion");
    tap_is_int(status, 0, "here-string: expansion exit status 0");
    free(out);
}

static void test_compound_redirect(void)
{
    int status;
    char *out;
    char *contents;

    /* Brace group with redirect */
    out = run("{ echo a; echo b; } > tmp/posix_group.txt", &status);
    tap_is_int(status, 0, "compound redir: exit status 0");
    tap_is_str(out, "", "compound redir: nothing on stdout");
    free(out);

    contents = read_file("tmp/posix_group.txt");
    tap_is_str(contents, "a\nb\n", "compound redir: file has both lines");
    free(contents);

    unlink("tmp/posix_group.txt");

    /* After redirect: stdout is restored */
    out = run("{ echo inside; } > /dev/null; echo outside", &status);
    tap_is_str(out, "outside\n", "compound redir: stdout restored after");
    tap_is_int(status, 0, "compound redir: restore exit status 0");
    free(out);

    /* If command with redirect */
    out = run("if true; then echo yes; fi > tmp/posix_group.txt; cat tmp/posix_group.txt", &status);
    tap_is_str(out, "yes\n", "compound redir: if > file");
    tap_is_int(status, 0, "compound redir: if exit status 0");
    free(out);

    unlink("tmp/posix_group.txt");

    /* For loop with redirect */
    out = run("for x in a b; do echo $x; done > tmp/posix_group.txt; cat tmp/posix_group.txt",
              &status);
    tap_is_str(out, "a\nb\n", "compound redir: for > file");
    tap_is_int(status, 0, "compound redir: for exit status 0");
    free(out);

    unlink("tmp/posix_group.txt");
}

/* Total: 32 assertions */

int main(void)
{
    tap_plan(32);
    ensure_tmp();

    test_output_redirect();
    test_append_redirect();
    test_heredoc_expansion();
    test_heredoc_quoted();
    test_heredoc_tab_strip();
    test_fd_duplication();
    test_here_string();
    test_compound_redirect();

    return tap_done();
}
