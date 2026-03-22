#include "../tap.h"
#include "parser/parser.h"

#include <stdlib.h>
#include <string.h>

static void test_simple_command(void)
{
    parser_t p;
    parser_init(&p, "echo hello world", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "simple command: parsed");
    tap_is_int(parser_error_count(&p), 0, "simple command: no errors");

    tap_ok(result->pipelines != NULL, "simple command: has pipeline");
    tap_ok(result->pipelines->commands != NULL, "simple command: has command");
    tap_is_int(result->pipelines->commands->type, CT_SIMPLE, "simple command: type");
    tap_is_int((long long)result->pipelines->commands->u.simple.words.length, 3,
               "simple command: 3 words");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_pipeline(void)
{
    parser_t p;
    parser_init(&p, "echo hello | cat | wc", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "pipeline: parsed");
    tap_is_int(parser_error_count(&p), 0, "pipeline: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_ok(cmd != NULL, "pipeline: first command");
    tap_ok(cmd->next != NULL, "pipeline: second command");
    tap_ok(cmd->next->next != NULL, "pipeline: third command");
    tap_ok(cmd->next->next->next == NULL, "pipeline: no fourth command");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_and_or(void)
{
    parser_t p;
    parser_init(&p, "true && echo yes || echo no", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "and-or: parsed");
    tap_is_int(parser_error_count(&p), 0, "and-or: no errors");

    and_or_t *pl = result->pipelines;
    tap_ok(pl != NULL, "and-or: first pipeline");
    tap_ok(pl->next != NULL, "and-or: second pipeline");
    tap_ok(pl->next->next != NULL, "and-or: third pipeline");
    tap_ok(pl->connector == true, "and-or: first joins with && (true)");
    tap_ok(pl->next->connector == false, "and-or: second joins with || (false)");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_if_command(void)
{
    parser_t p;
    parser_init(&p, "if true; then echo yes; elif false; then echo maybe; else echo no; fi",
                "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "if: parsed");
    tap_is_int(parser_error_count(&p), 0, "if: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_IF, "if: type is CT_IF");

    if_clause_t *ic = cmd->u.if_clause.clauses;
    tap_ok(ic != NULL, "if: has first clause");
    tap_ok(ic->condition != NULL, "if: first clause has condition");
    tap_ok(ic->next != NULL, "if: has elif clause");
    tap_ok(ic->next->condition != NULL, "if: elif has condition");
    tap_ok(ic->next->next != NULL, "if: has else clause");
    tap_ok(ic->next->next->condition == NULL, "if: else has no condition");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_for_command(void)
{
    parser_t p;
    parser_init(&p, "for x in a b c; do echo $x; done", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "for: parsed");
    tap_is_int(parser_error_count(&p), 0, "for: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_FOR, "for: type is CT_FOR");
    tap_is_str(cmd->u.for_clause.varname, "x", "for: variable is x");
    tap_is_int((long long)cmd->u.for_clause.wordlist.length, 3, "for: 3 words in list");
    tap_ok(cmd->u.for_clause.body != NULL, "for: has body");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_while_command(void)
{
    parser_t p;
    parser_init(&p, "while true; do echo loop; done", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "while: parsed");
    tap_is_int(parser_error_count(&p), 0, "while: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_WHILE, "while: type is CT_WHILE");
    tap_ok(cmd->u.while_clause.condition != NULL, "while: has condition");
    tap_ok(cmd->u.while_clause.body != NULL, "while: has body");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_case_command(void)
{
    parser_t p;
    parser_init(&p, "case $x in\na) echo a;;\nb|c) echo bc;;\n*) echo other;;\nesac", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "case: parsed");
    tap_is_int(parser_error_count(&p), 0, "case: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_CASE, "case: type is CT_CASE");
    tap_ok(cmd->u.case_clause.subject != NULL, "case: has case word");

    case_item_t *ci = cmd->u.case_clause.items;
    tap_ok(ci != NULL, "case: has first item");
    tap_is_int((long long)ci->patterns.length, 1, "case: first item has 1 pattern");
    tap_ok(ci->next != NULL, "case: has second item");
    tap_is_int((long long)ci->next->patterns.length, 2, "case: second item has 2 patterns");
    tap_ok(ci->next->next != NULL, "case: has third item");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_brace_group(void)
{
    parser_t p;
    parser_init(&p, "{ echo hello; echo world; }", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "brace group: parsed");
    tap_is_int(parser_error_count(&p), 0, "brace group: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_GROUP, "brace group: type is CT_GROUP");
    tap_ok(cmd->u.group.body != NULL, "brace group: has body");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_subshell(void)
{
    parser_t p;
    parser_init(&p, "(echo hello; echo world)", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "subshell: parsed");
    tap_is_int(parser_error_count(&p), 0, "subshell: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_SUBSHELL, "subshell: type is CT_SUBSHELL");
    tap_ok(cmd->u.group.body != NULL, "subshell: has body");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_function_def(void)
{
    parser_t p;
    parser_init(&p, "myfunc() { echo hello; }", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "funcdef: parsed");
    tap_is_int(parser_error_count(&p), 0, "funcdef: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_FUNCDEF, "funcdef: type is CT_FUNCDEF");
    tap_is_str(cmd->u.func_def.name, "myfunc", "funcdef: name is myfunc");
    tap_ok(cmd->u.func_def.body != NULL, "funcdef: has body");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_function_keyword(void)
{
    parser_t p;
    parser_init(&p, "function myfunc { echo hello; }", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "function keyword: parsed");
    tap_is_int(parser_error_count(&p), 0, "function keyword: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_FUNCDEF, "function keyword: type is CT_FUNCDEF");
    tap_is_str(cmd->u.func_def.name, "myfunc", "function keyword: name");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_assignment(void)
{
    parser_t p;
    parser_init(&p, "VAR=hello OTHER=world echo $VAR", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "assignment: parsed");
    tap_is_int(parser_error_count(&p), 0, "assignment: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_is_int(cmd->type, CT_SIMPLE, "assignment: type is CT_SIMPLE");
    tap_is_int((long long)cmd->u.simple.assigns.length, 2, "assignment: 2 assignments");
    tap_is_int((long long)cmd->u.simple.words.length, 2, "assignment: 2 words");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_redirection(void)
{
    parser_t p;
    parser_init(&p, "echo hello >file 2>&1", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "redirection: parsed");
    tap_is_int(parser_error_count(&p), 0, "redirection: no errors");

    command_t *cmd = result->pipelines->commands;
    tap_ok(cmd->redirs != NULL, "redirection: has redirections");
    tap_is_int(cmd->redirs->type, REDIR_OUT, "redirection: first is output");
    tap_is_int(cmd->redirs->fd, 1, "redirection: fd 1");
    tap_ok(cmd->redirs->next != NULL, "redirection: has second");
    tap_is_int(cmd->redirs->next->fd, 2, "redirection: second fd is 2");

    sh_list_free(result);
    parser_destroy(&p);
}

static void test_bracket(void)
{
    /* Simple unary test */
    {
        parser_t p;
        parser_init(&p, "[[ -f file ]]", "test");
        sh_list_t *result = parser_parse(&p);
        tap_ok(result != NULL, "bracket simple: parsed");
        tap_is_int(parser_error_count(&p), 0, "bracket simple: no errors");
        if (result != NULL) {
            command_t *cmd = result->pipelines->commands;
            tap_is_int(cmd->type, CT_BRACKET, "bracket simple: type");
            tap_ok(cmd->u.cond.expr != NULL, "bracket simple: has expr");
            tap_is_int(cmd->u.cond.expr->type, COND_UNARY, "bracket simple: unary");
        } else {
            tap_skip(3, "parse failed");
        }
        sh_list_free(result);
        parser_destroy(&p);
    }

    /* Binary test with && */
    {
        parser_t p;
        parser_init(&p, "[[ -f file && x == hello ]]", "test");
        sh_list_t *result = parser_parse(&p);
        tap_ok(result != NULL, "bracket and: parsed");
        tap_is_int(parser_error_count(&p), 0, "bracket and: no errors");
        if (result != NULL) {
            command_t *cmd = result->pipelines->commands;
            tap_is_int(cmd->type, CT_BRACKET, "bracket and: type");
            tap_ok(cmd->u.cond.expr != NULL, "bracket and: has expr");
            tap_is_int(cmd->u.cond.expr->type, COND_AND, "bracket and: is AND");
        } else {
            tap_skip(3, "parse failed");
        }
        sh_list_free(result);
        parser_destroy(&p);
    }
}

static void test_parse_error(void)
{
    parser_t p;
    parser_init(&p, "if true; then echo yes", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(parser_error_count(&p) > 0, "parse error: missing fi detected");
    sh_list_free(result);
    parser_destroy(&p);
}

static void test_empty_input(void)
{
    parser_t p;
    parser_init(&p, "", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result == NULL, "empty input: returns NULL");
    tap_is_int(parser_error_count(&p), 0, "empty input: no errors");

    parser_destroy(&p);
}

static void test_multiple_commands(void)
{
    parser_t p;
    parser_init(&p, "echo a; echo b; echo c", "test");

    sh_list_t *result = parser_parse(&p);
    tap_ok(result != NULL, "multi-cmd: parsed");
    tap_is_int(parser_error_count(&p), 0, "multi-cmd: no errors");

    tap_ok(result->next != NULL, "multi-cmd: has second");
    tap_ok(result->next->next != NULL, "multi-cmd: has third");
    tap_ok(result->next->next->next == NULL, "multi-cmd: no fourth");

    sh_list_free(result);
    parser_destroy(&p);
}

int main(void)
{
    tap_plan(95);

    test_simple_command();
    test_pipeline();
    test_and_or();
    test_if_command();
    test_for_command();
    test_while_command();
    test_case_command();
    test_brace_group();
    test_subshell();
    test_function_def();
    test_function_keyword();
    test_assignment();
    test_redirection();
    test_bracket();
    test_parse_error();
    test_empty_input();
    test_multiple_commands();

    return tap_done();
}
