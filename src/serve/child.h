#ifndef OPSH_SERVE_CHILD_H
#define OPSH_SERVE_CHILD_H

/* Run the child command loop: read length-prefixed commands from stdin,
 * execute each in a persistent VM context, signal completion on fd 3. */
int child_loop_main(void);

#endif /* OPSH_SERVE_CHILD_H */
