#ifndef OPSH_LSP_LSP_H
#define OPSH_LSP_LSP_H

/* Run the LSP server (blocking read loop on stdin, writes to stdout) */
int lsp_main(void);

/* Handle a single JSON-RPC message. Returns 1 to continue, 0 to stop. */
int lsp_handle_message(const char *msg);

#endif /* OPSH_LSP_LSP_H */
