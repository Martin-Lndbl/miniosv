/*
 * Console geometry, for applications that render to the terminal.
 *
 * miniOSv has no ioctl: the terminal ioctls used to exist only so libc's
 * tcgetattr()/tcsetattr() could reach the console, and those now call it
 * directly. The one thing application code still asks a terminal for is its
 * size, so it asks for that and nothing else.
 */

#ifndef OSV_TERMINAL_H
#define OSV_TERMINAL_H

#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills *ws with the console's rows and columns. Returns 0, or -1 with errno
 * set if ws is null. */
int osv_terminal_size(struct winsize *ws);

#ifdef __cplusplus
}
#endif

#endif /* OSV_TERMINAL_H */
