#include "foundation/util.h"

#include "foundation/strbuf.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void *xmalloc(size_t size)
{
    void *p = malloc(size);
    if (p == NULL && size > 0) {
        fprintf(stderr, "opsh: out of memory (malloc %zu bytes)\n", size);
        abort();
    }
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (p == NULL && size > 0) {
        fprintf(stderr, "opsh: out of memory (realloc %zu bytes)\n", size);
        abort();
    }
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count, size);
    if (p == NULL && count > 0 && size > 0) {
        fprintf(stderr, "opsh: out of memory (calloc %zu * %zu bytes)\n", count, size);
        abort();
    }
    return p;
}

int checked_add(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b) {
        return -1;
    }
    *result = a + b;
    return 0;
}

int checked_mul(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return -1;
    }
    *result = a * b;
    return 0;
}

char *xstrdup(const char *s)
{
    size_t len = strlen(s);
    char *dup = xmalloc(len + 1);
    memcpy(dup, s, len + 1);
    return dup;
}

char *read_file(const char *path)
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

char *read_stdin(void)
{
    strbuf_t buf;
    strbuf_init(&buf);
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), stdin)) > 0) {
        strbuf_append_bytes(&buf, tmp, n);
    }
    return strbuf_detach(&buf);
}

char *get_self_exe(void)
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
