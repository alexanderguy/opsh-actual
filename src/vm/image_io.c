#include "vm/image_io.h"

#include "foundation/util.h"

#include <stdlib.h>
#include <string.h>

/* Sanity limits for untrusted .opsb input */
#define OPSB_MAX_SECTION_SIZE (64 * 1024 * 1024)   /* 64 MB */
#define OPSB_MAX_STRING_SIZE (16 * 1024 * 1024)     /* 16 MB */
#define OPSB_MAX_SECTIONS 256

/* Write helpers (little-endian) */
static void write_u8(FILE *f, uint8_t v)
{
    fputc(v, f);
}

static void write_u16(FILE *f, uint16_t v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void write_u32(FILE *f, uint32_t v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >> 24) & 0xFF, f);
}

/* Read context: tracks whether an error/EOF has occurred */
typedef struct {
    FILE *f;
    bool error;
} read_ctx_t;

static uint8_t ctx_read_u8(read_ctx_t *ctx)
{
    if (ctx->error) {
        return 0;
    }
    int c = fgetc(ctx->f);
    if (c == EOF) {
        ctx->error = true;
        return 0;
    }
    return (uint8_t)c;
}

static uint16_t ctx_read_u16(read_ctx_t *ctx)
{
    uint16_t lo = ctx_read_u8(ctx);
    uint16_t hi = ctx_read_u8(ctx);
    return lo | (hi << 8);
}

static uint32_t ctx_read_u32(read_ctx_t *ctx)
{
    uint32_t b0 = ctx_read_u8(ctx);
    uint32_t b1 = ctx_read_u8(ctx);
    uint32_t b2 = ctx_read_u8(ctx);
    uint32_t b3 = ctx_read_u8(ctx);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static bool ctx_read_bytes(read_ctx_t *ctx, void *buf, size_t n)
{
    if (ctx->error) {
        return false;
    }
    if (n > 0 && fread(buf, 1, n, ctx->f) != n) {
        ctx->error = true;
        return false;
    }
    return true;
}

/* Find a string in the constant pool, returning its index or -1 */
static int find_const(const bytecode_image_t *img, const char *name)
{
    uint16_t i;
    for (i = 0; i < img->const_count; i++) {
        if (strcmp(img->const_pool[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Section size helpers */
static uint32_t const_pool_size(const bytecode_image_t *img)
{
    uint32_t size = 2; /* u16 count */
    uint16_t i;
    for (i = 0; i < img->const_count; i++) {
        size += 4 + (uint32_t)strlen(img->const_pool[i]);
    }
    return size;
}

static uint32_t func_table_size(const bytecode_image_t *img)
{
    return 2 + (uint32_t)img->func_count * 6;
}

static uint32_t module_table_size(const bytecode_image_t *img)
{
    return 2 + (uint32_t)img->module_count * 6;
}

int image_write_opsb(const bytecode_image_t *img, FILE *out)
{
    uint32_t section_count = 2; /* const pool + bytecode always present */
    if (img->func_count > 0) {
        section_count++;
    }
    if (img->module_count > 0) {
        section_count++;
    }

    /* Header */
    fwrite(OPSB_MAGIC, 1, 4, out);
    write_u16(out, OPSB_VERSION);
    write_u16(out, 0); /* flags */
    write_u32(out, section_count);
    write_u32(out, 0); /* reserved */

    /* Section: constant pool */
    write_u8(out, SECT_CONST_POOL);
    write_u32(out, const_pool_size(img));
    write_u16(out, img->const_count);
    {
        uint16_t i;
        for (i = 0; i < img->const_count; i++) {
            uint32_t len = (uint32_t)strlen(img->const_pool[i]);
            write_u32(out, len);
            fwrite(img->const_pool[i], 1, len, out);
        }
    }

    /* Section: bytecode */
    write_u8(out, SECT_BYTECODE);
    write_u32(out, (uint32_t)img->code_size);
    fwrite(img->code, 1, img->code_size, out);

    /* Section: function table */
    if (img->func_count > 0) {
        write_u8(out, SECT_FUNC_TABLE);
        write_u32(out, func_table_size(img));
        write_u16(out, (uint16_t)img->func_count);
        {
            int i;
            for (i = 0; i < img->func_count; i++) {
                int idx = find_const(img, img->funcs[i].name);
                if (idx < 0) {
                    fprintf(stderr, "opsh: function name '%s' not in constant pool\n",
                            img->funcs[i].name);
                    return -1;
                }
                write_u16(out, (uint16_t)idx);
                write_u32(out, (uint32_t)img->funcs[i].bytecode_offset);
            }
        }
    }

    /* Section: module table */
    if (img->module_count > 0) {
        write_u8(out, SECT_MODULE_TABLE);
        write_u32(out, module_table_size(img));
        write_u16(out, (uint16_t)img->module_count);
        {
            int i;
            for (i = 0; i < img->module_count; i++) {
                int idx = find_const(img, img->modules[i].name);
                if (idx < 0) {
                    fprintf(stderr, "opsh: module name '%s' not in constant pool\n",
                            img->modules[i].name);
                    return -1;
                }
                write_u16(out, (uint16_t)idx);
                write_u32(out, (uint32_t)img->modules[i].init_offset);
            }
        }
    }

    return ferror(out) ? -1 : 0;
}

bytecode_image_t *image_read_opsb(FILE *in)
{
    read_ctx_t ctx = {in, false};

    /* Read and validate header */
    char magic[4];
    if (!ctx_read_bytes(&ctx, magic, 4) || memcmp(magic, OPSB_MAGIC, 4) != 0) {
        fprintf(stderr, "opsh: not a valid .opsb file\n");
        return NULL;
    }

    uint16_t version = ctx_read_u16(&ctx);
    if (ctx.error || version != OPSB_VERSION) {
        fprintf(stderr, "opsh: unsupported .opsb version %u\n", version);
        return NULL;
    }

    ctx_read_u16(&ctx); /* flags */
    uint32_t section_count = ctx_read_u32(&ctx);
    ctx_read_u32(&ctx); /* reserved */

    if (ctx.error) {
        fprintf(stderr, "opsh: truncated .opsb header\n");
        return NULL;
    }

    if (section_count > OPSB_MAX_SECTIONS) {
        fprintf(stderr, "opsh: too many sections in .opsb (%u)\n", section_count);
        return NULL;
    }

    bytecode_image_t *img = image_new();
    uint32_t si;
    bool has_const_pool = false;
    bool has_bytecode = false;
    bool has_func_table = false;
    bool has_module_table = false;

    for (si = 0; si < section_count && !ctx.error; si++) {
        uint8_t sect_type = ctx_read_u8(&ctx);
        uint32_t sect_len = ctx_read_u32(&ctx);

        if (ctx.error) {
            break;
        }

        if (sect_len > OPSB_MAX_SECTION_SIZE) {
            fprintf(stderr, "opsh: .opsb section too large (%u bytes)\n", sect_len);
            image_free(img);
            return NULL;
        }

        switch (sect_type) {
        case SECT_CONST_POOL: {
            if (has_const_pool) {
                fprintf(stderr, "opsh: duplicate const pool in .opsb\n");
                image_free(img);
                return NULL;
            }
            uint16_t count = ctx_read_u16(&ctx);
            uint16_t ci;
            for (ci = 0; ci < count && !ctx.error; ci++) {
                uint32_t slen = ctx_read_u32(&ctx);
                if (slen > OPSB_MAX_STRING_SIZE) {
                    fprintf(stderr, "opsh: .opsb string too large (%u bytes)\n", slen);
                    image_free(img);
                    return NULL;
                }
                char *str = xmalloc(slen + 1);
                if (!ctx_read_bytes(&ctx, str, slen)) {
                    free(str);
                    break;
                }
                str[slen] = '\0';
                image_add_const(img, str);
                free(str);
            }
            has_const_pool = true;
            break;
        }

        case SECT_BYTECODE: {
            if (has_bytecode) {
                fprintf(stderr, "opsh: duplicate bytecode section in .opsb\n");
                image_free(img);
                return NULL;
            }
            has_bytecode = true;
            img->code = xmalloc(sect_len);
            img->code_size = sect_len;
            img->code_cap = sect_len;
            if (!ctx_read_bytes(&ctx, img->code, sect_len)) {
                break;
            }
            break;
        }

        case SECT_FUNC_TABLE: {
            if (!has_const_pool) {
                fprintf(stderr, "opsh: func table before const pool in .opsb\n");
                image_free(img);
                return NULL;
            }
            if (has_func_table) {
                fprintf(stderr, "opsh: duplicate func table in .opsb\n");
                image_free(img);
                return NULL;
            }
            has_func_table = true;
            uint16_t count = ctx_read_u16(&ctx);
            img->funcs = xcalloc((size_t)count, sizeof(vm_func_t));
            {
                int fi;
                for (fi = 0; fi < count && !ctx.error; fi++) {
                    uint16_t name_idx = ctx_read_u16(&ctx);
                    uint32_t offset = ctx_read_u32(&ctx);
                    if (ctx.error) {
                        break;
                    }
                    if (name_idx < img->const_count) {
                        char *name = xmalloc(strlen(img->const_pool[name_idx]) + 1);
                        strcpy(name, img->const_pool[name_idx]);
                        img->funcs[fi].name = name;
                    } else {
                        char *name = xmalloc(1);
                        name[0] = '\0';
                        img->funcs[fi].name = name;
                    }
                    img->funcs[fi].bytecode_offset = offset;
                    img->func_count = fi + 1;
                }
            }
            break;
        }

        case SECT_MODULE_TABLE: {
            if (!has_const_pool) {
                fprintf(stderr, "opsh: module table before const pool in .opsb\n");
                image_free(img);
                return NULL;
            }
            if (has_module_table) {
                fprintf(stderr, "opsh: duplicate module table in .opsb\n");
                image_free(img);
                return NULL;
            }
            has_module_table = true;
            uint16_t count = ctx_read_u16(&ctx);
            img->modules = xcalloc((size_t)count, sizeof(vm_module_t));
            {
                int mi;
                for (mi = 0; mi < count && !ctx.error; mi++) {
                    uint16_t name_idx = ctx_read_u16(&ctx);
                    uint32_t offset = ctx_read_u32(&ctx);
                    if (ctx.error) {
                        break;
                    }
                    if (name_idx < img->const_count) {
                        char *name = xmalloc(strlen(img->const_pool[name_idx]) + 1);
                        strcpy(name, img->const_pool[name_idx]);
                        img->modules[mi].name = name;
                    } else {
                        char *name = xmalloc(1);
                        name[0] = '\0';
                        img->modules[mi].name = name;
                    }
                    img->modules[mi].init_offset = offset;
                    img->module_count = mi + 1;
                }
            }
            break;
        }

        default:
            /* Skip unknown section by reading and discarding bytes */
            if (sect_len > 0) {
                void *skip_buf = xmalloc(sect_len);
                ctx_read_bytes(&ctx, skip_buf, sect_len);
                free(skip_buf);
            }
            break;
        }
    }

    if (ctx.error) {
        fprintf(stderr, "opsh: truncated or corrupt .opsb file\n");
        image_free(img);
        return NULL;
    }

    return img;
}
