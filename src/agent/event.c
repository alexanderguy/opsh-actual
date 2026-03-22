#include "agent/event.h"

#include "foundation/json.h"
#include "foundation/strbuf.h"
#include "foundation/util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No-op sink */

static void none_emit(event_sink_t *sink, const event_t *event)
{
    (void)sink;
    (void)event;
}

static void none_destroy(event_sink_t *sink)
{
    free(sink);
}

event_sink_t *event_sink_none(void)
{
    event_sink_t *sink = xcalloc(1, sizeof(*sink));
    sink->emit = none_emit;
    sink->destroy = none_destroy;
    return sink;
}

/* JSON-RPC stdio sink */

static const char *event_type_name(event_type_t type)
{
    switch (type) {
    case EVENT_SCRIPT_START:
        return "scriptStart";
    case EVENT_SCRIPT_END:
        return "scriptEnd";
    case EVENT_COMMAND_START:
        return "commandStart";
    case EVENT_COMMAND_END:
        return "commandEnd";
    case EVENT_ERROR:
        return "error";
    }
    return "unknown";
}

static void write_content_length(int fd, const char *json, size_t len)
{
    char header[64];
    int hlen = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
    {
        size_t written = 0;
        while (written < (size_t)hlen) {
            ssize_t n = write(fd, header + written, (size_t)hlen - written);
            if (n > 0) {
                written += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
    {
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(fd, json + written, len - written);
            if (n > 0) {
                written += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
}

static void stdio_emit(event_sink_t *sink, const event_t *event)
{
    int fd = (int)(intptr_t)sink->data;
    strbuf_t buf;
    strbuf_init(&buf);

    json_begin_object(&buf);
    json_key_string(&buf, "jsonrpc", "2.0");
    json_key_string(&buf, "method", event_type_name(event->type));

    /* params object */
    strbuf_append_str(&buf, ",\"params\":{");

    switch (event->type) {
    case EVENT_SCRIPT_START:
        if (event->filename != NULL) {
            json_key_string(&buf, "file", event->filename);
        }
        break;
    case EVENT_SCRIPT_END:
        json_key_int(&buf, "status", event->status);
        break;
    case EVENT_COMMAND_START:
        json_key_int(&buf, "id", event->id);
        if (event->name != NULL) {
            json_key_string(&buf, "name", event->name);
        }
        break;
    case EVENT_COMMAND_END:
        json_key_int(&buf, "id", event->id);
        json_key_int(&buf, "status", event->status);
        break;
    case EVENT_ERROR:
        if (event->message != NULL) {
            json_key_string(&buf, "message", event->message);
        }
        break;
    }

    strbuf_append_byte(&buf, '}'); /* close params */
    json_end_object(&buf);

    write_content_length(fd, buf.contents, buf.length);
    strbuf_destroy(&buf);
}

static void stdio_destroy(event_sink_t *sink)
{
    int fd = (int)(intptr_t)sink->data;
    if (fd >= 0) {
        close(fd);
    }
    free(sink);
}

event_sink_t *event_sink_stdio(int fd)
{
    event_sink_t *sink = xcalloc(1, sizeof(*sink));
    sink->emit = stdio_emit;
    sink->destroy = stdio_destroy;
    sink->data = (void *)(intptr_t)fd;
    return sink;
}
