#ifndef OPSH_SERVE_SERVE_H
#define OPSH_SERVE_SERVE_H

/* Start the session management server.
 * Reads JSON-RPC 2.0 over Content-Length framed stdin,
 * writes responses to stdout. */
int serve_main(void);

/* Handle a single JSON-RPC message. Returns 1 to continue, 0 to stop.
 * Exposed for testing. */
int serve_handle_message(const char *msg);

#endif /* OPSH_SERVE_SERVE_H */
