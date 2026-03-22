#ifndef OPSH_AGENT_EVENT_H
#define OPSH_AGENT_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Event types emitted by the VM during execution.
 */
typedef enum {
    EVENT_SCRIPT_START,
    EVENT_SCRIPT_END,
    EVENT_COMMAND_START,
    EVENT_COMMAND_END,
    EVENT_ERROR,
} event_type_t;

/*
 * Event data.
 */
typedef struct {
    event_type_t type;
    int64_t id;           /* command ID (for COMMAND_START/END) */
    const char *name;     /* command name (for COMMAND_START) */
    int status;           /* exit status (for COMMAND_END, SCRIPT_END) */
    const char *message;  /* error message (for ERROR) */
    const char *filename; /* script filename (for SCRIPT_START) */
} event_t;

/*
 * Event sink: receives events from the VM.
 */
typedef struct event_sink {
    void (*emit)(struct event_sink *sink, const event_t *event);
    void (*destroy)(struct event_sink *sink);
    void *data; /* sink-specific data */
} event_sink_t;

/* Create a no-op event sink */
event_sink_t *event_sink_none(void);

/* Create a JSON-RPC stdio event sink (writes to the given fd) */
event_sink_t *event_sink_stdio(int fd);

/* Emit an event to a sink (NULL-safe) */
static inline void event_emit(event_sink_t *sink, const event_t *event)
{
    if (sink != NULL && sink->emit != NULL) {
        sink->emit(sink, event);
    }
}

/* Destroy a sink (NULL-safe) */
static inline void event_sink_free(event_sink_t *sink)
{
    if (sink != NULL) {
        if (sink->destroy != NULL) {
            sink->destroy(sink);
        }
    }
}

#endif /* OPSH_AGENT_EVENT_H */
