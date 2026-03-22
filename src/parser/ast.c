#include "parser/ast.h"

#include <stdlib.h>

void param_exp_free(param_exp_t *p)
{
    if (p == NULL) {
        return;
    }
    free(p->name);
    word_part_free(p->pattern);
    word_part_free(p->replacement);
    /* node itself is arena-allocated */
}

void cmd_subst_free(cmd_subst_t *ec)
{
    if (ec->is_preparsed) {
        sh_list_free(ec->u.preparsed);
    } else {
        free(ec->u.unparsed);
    }
}

void word_part_free(word_part_t *w)
{
    while (w != NULL) {
        word_part_t *next = w->next;
        switch (w->type) {
        case WP_LITERAL:
            free(w->part.string);
            break;
        case WP_PARAM:
            param_exp_free(w->part.param);
            break;
        case WP_CMDSUB:
            cmd_subst_free(&w->part.cmdsub);
            break;
        case WP_ARITH:
            word_part_free(w->part.arith);
            break;
        }
        /* node itself is arena-allocated */
        w = next;
    }
}

void io_redir_free(io_redir_t *r)
{
    while (r != NULL) {
        io_redir_t *next = r->next;
        word_part_free(r->target);
        free(r);
        r = next;
    }
}

void cond_expr_free(cond_expr_t *d)
{
    if (d == NULL) {
        return;
    }
    switch (d->type) {
    case COND_AND:
    case COND_OR:
        cond_expr_free(d->u.andor.left);
        cond_expr_free(d->u.andor.right);
        break;
    case COND_NOT:
        cond_expr_free(d->u.not.child);
        break;
    case COND_UNARY:
        free(d->u.unary.op);
        word_part_free(d->u.unary.arg);
        break;
    case COND_BINARY:
        free(d->u.binary.op);
        word_part_free(d->u.binary.left);
        word_part_free(d->u.binary.right);
        break;
    case COND_STRING:
        word_part_free(d->u.string.word);
        break;
    }
    free(d);
}

void if_clause_free(if_clause_t *ic)
{
    while (ic != NULL) {
        if_clause_t *next = ic->next;
        sh_list_free(ic->condition);
        sh_list_free(ic->body);
        free(ic);
        ic = next;
    }
}

void case_item_free(case_item_t *ci)
{
    while (ci != NULL) {
        case_item_t *next = ci->next;
        size_t i;
        for (i = 0; i < ci->patterns.length; i++) {
            word_part_free(plist_get(&ci->patterns, i));
        }
        plist_destroy(&ci->patterns);
        sh_list_free(ci->body);
        free(ci);
        ci = next;
    }
}

static void command_free_contents(command_t *c)
{
    size_t i;

    io_redir_free(c->redirs);

    switch (c->type) {
    case CT_SIMPLE:
        for (i = 0; i < c->u.simple.assigns.length; i++) {
            word_part_free(plist_get(&c->u.simple.assigns, i));
        }
        plist_destroy(&c->u.simple.assigns);
        for (i = 0; i < c->u.simple.words.length; i++) {
            word_part_free(plist_get(&c->u.simple.words, i));
        }
        plist_destroy(&c->u.simple.words);
        break;
    case CT_GROUP:
    case CT_SUBSHELL:
        sh_list_free(c->u.group.body);
        break;
    case CT_IF:
        if_clause_free(c->u.if_clause.clauses);
        break;
    case CT_FOR:
        free(c->u.for_clause.varname);
        for (i = 0; i < c->u.for_clause.wordlist.length; i++) {
            word_part_free(plist_get(&c->u.for_clause.wordlist, i));
        }
        plist_destroy(&c->u.for_clause.wordlist);
        sh_list_free(c->u.for_clause.body);
        break;
    case CT_WHILE:
    case CT_UNTIL:
        sh_list_free(c->u.while_clause.condition);
        sh_list_free(c->u.while_clause.body);
        break;
    case CT_CASE:
        word_part_free(c->u.case_clause.subject);
        case_item_free(c->u.case_clause.items);
        break;
    case CT_FUNCDEF:
        free(c->u.func_def.name);
        command_free(c->u.func_def.body);
        break;
    case CT_BRACKET:
        cond_expr_free(c->u.cond.expr);
        break;
    }
}

void command_free(command_t *c)
{
    while (c != NULL) {
        command_t *next = c->next;
        if (refcount_dec(&c->refcount)) {
            command_free_contents(c);
            free(c);
        }
        c = next;
    }
}

void and_or_free(and_or_t *p)
{
    while (p != NULL) {
        and_or_t *next = p->next;
        command_free(p->commands);
        free(p);
        p = next;
    }
}

void sh_list_free(sh_list_t *a)
{
    while (a != NULL) {
        sh_list_t *next = a->next;
        and_or_free(a->pipelines);
        free(a);
        a = next;
    }
}
