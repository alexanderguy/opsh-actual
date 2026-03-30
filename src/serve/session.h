#ifndef OPSH_SERVE_SESSION_H
#define OPSH_SERVE_SESSION_H

#define SESSION_DEFAULT_TIMEOUT_MS 30000
#define SESSION_MAX_TIMEOUT_MS 300000

#define SESSION_ERR (-1)
#define SESSION_ERR_INVALID (-2)

typedef struct {
    int session_id;
} session_create_result_t;

typedef struct {
    int exit_status;
    char *out_buf; /* caller frees, or use session_eval_result_destroy */
    char *err_buf; /* caller frees, or use session_eval_result_destroy */
    int truncated;
} session_eval_result_t;

void session_eval_result_destroy(session_eval_result_t *r);

/* Callback for streaming output during eval. stream_name is "stdout" or
 * "stderr", data is a null-terminated chunk of new output. */
typedef void (*session_stream_cb)(const char *stream_name, const char *data, void *ctx);

typedef struct {
    int *session_ids; /* caller frees */
    int count;
} session_list_result_t;

/*
 * All ops return 0 on success, negative on error:
 *   SESSION_ERR (-1) — internal/server error
 *   SESSION_ERR_INVALID (-2) — bad input from caller
 * On error, *error is set to a malloc'd string (caller frees).
 * On success, *error is always NULL.
 *
 * session_init must be called once before any session_op_* call.
 * session_cleanup closes all sessions and releases resources.
 * These operate on process-global state — only one init/cleanup pair.
 */
int session_init(void);
void session_cleanup(void);

int session_op_create(const char *cwd, session_create_result_t *out, char **error);
int session_op_eval(int session_id, const char *source, int timeout_ms, session_stream_cb stream_cb,
                    void *stream_ctx, session_eval_result_t *out, char **error);
int session_op_signal(int session_id, int signum, char **error);
int session_op_destroy(int session_id, char **error);
int session_op_list(session_list_result_t *out, char **error);

#endif /* OPSH_SERVE_SESSION_H */
