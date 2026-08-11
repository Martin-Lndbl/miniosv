/*
 * vAccel: offloading an operation to an accelerator on the host.
 *
 * This is the API llama.cpp's ggml-vaccel backend is written against, and the
 * headers beside it are copied unchanged from the Unikraft port
 * (lros-unikraft/lib/libvaccelrt/include) because their layouts and enum
 * values are the wire format -- the device reads these structures.
 *
 * What is *not* here is the two layers Unikraft needs and miniOSv does not.
 * There, a call reaches the device by opening /dev/accel and issuing an ioctl
 * on it; that exists only to cross a kernel/user boundary. Here the
 * implementation calls drivers/virtio-accel.hh directly, in the same spirit as
 * the application calling modules/miniext rather than going through a VFS.
 *
 * Only the operations the backend actually issues are implemented: sessions
 * and the matmul family. The upstream header also declares image
 * classification, exec, minmax and BLAS; nothing here calls them, so they are
 * left out rather than stubbed.
 */

#ifndef __VACCEL_H__
#define __VACCEL_H__

#include "error.h"
#include "session.h"
#include "ops/vaccel_ops.h"
#include "ops/matmul.h"

#endif /* __VACCEL_H__ */
