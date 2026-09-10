/*
 * VACCEL_EXEC: run a named function on the host plugin with arbitrary
 * arguments. The host's genop strips the opcode, vaccel_exec_unpack pops the
 * library and symbol names, and everything after them reaches the plugin
 * untouched. That makes this the carrier for operations vAccel core has no
 * opcode for.
 */

#ifndef __VACCEL_EXEC_H__
#define __VACCEL_EXEC_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vaccel_session;

//! One argument in an exec payload. Mirrors the host's struct vaccel_arg
//! field order so the two agree on the wire.
struct vaccel_arg {
	uint32_t argtype;
	uint32_t size;
	void *buf;
};

int vaccel_exec(struct vaccel_session *sess, const char *library,
		const char *fn_symbol, struct vaccel_arg *read, size_t nr_read,
		struct vaccel_arg *write, size_t nr_write);

#ifdef __cplusplus
}
#endif

#endif /* __VACCEL_EXEC_H__ */
