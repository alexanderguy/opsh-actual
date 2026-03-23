#ifndef OPSH_SERVE_MCP_H
#define OPSH_SERVE_MCP_H

/* Run the MCP server (blocking read loop on stdin, writes to stdout) */
int mcp_main(void);

/* Handle a single MCP message. Returns 1 to continue, 0 to stop.
 * Exposed for testing and fuzzing. */
int mcp_handle_message(const char *msg);

#endif /* OPSH_SERVE_MCP_H */
