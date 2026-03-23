#ifndef OPSH_FOUNDATION_JSONRPC_H
#define OPSH_FOUNDATION_JSONRPC_H

#include <stdint.h>
#include <stdio.h>

/* Read a Content-Length framed message from a stream.
 * Returns malloc'd body or NULL on EOF. */
char *jsonrpc_read_message(FILE *in);

/* Write a Content-Length framed JSON message to a stream. */
void jsonrpc_send(FILE *out, const char *json);

/* Build and send a JSON-RPC 2.0 result response. */
void jsonrpc_send_result(FILE *out, int64_t id, const char *result_json);

/* Build and send a JSON-RPC 2.0 error response. */
void jsonrpc_send_error(FILE *out, int64_t id, int code, const char *message);

#endif /* OPSH_FOUNDATION_JSONRPC_H */
