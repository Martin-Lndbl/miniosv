/*
 * The vAccel operations, packed onto the virtio-accel transport.
 *
 * Every call has the same shape: a list of arguments the host reads, a list it
 * writes, and an opcode as the first read argument. The device copies each
 * argument out of its own descriptor, so what matters is the order and the
 * lengths -- see drivers/virtio-accel.hh for the descriptor layout, and
 * lros-unikraft/lib/libvaccelrt/operations_matmul.c for where these argument
 * lists come from. They have to match the host plugin's expectations exactly,
 * so the order below is copied rather than invented.
 *
 * Handles (vaccel_tensor_mem_handle *, vaccel_matmul_ctx) are opaque host-side
 * tokens. Where an argument is a handle, it is the pointer *value* that
 * travels, not what it points at -- passing &handle sends the token.
 */

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "drivers/virtio-accel.hh"

#include "include/vaccel.h"

namespace {

using arg = virtio::accel::arg;

//! Translate a driver result into the return convention the vAccel API uses:
//! 0 on success, a positive errno-shaped code otherwise.
int to_vaccel(int rc)
{
    switch (rc) {
    case 0:        return VACCEL_OK;
    case -EINVAL:  return VACCEL_EINVAL;
    case -ENOMEM:  return VACCEL_ENOMEM;
    case -ENODEV:  return VACCEL_ENODEV;
    case -ENOSPC:  return VACCEL_EBUSY;
    default:       return VACCEL_EIO;
    }
}

//! One operation on the session's device, or VACCEL_ENODEV if the guest was
//! booted without one.
int op(const vaccel_session *sess, const arg *out, uint32_t out_nr,
       const arg *in, uint32_t in_nr)
{
    auto *dev = virtio::accel::instance();
    if (!dev || !sess) {
        return VACCEL_ENODEV;
    }
    return to_vaccel(dev->do_op(sess->session_id, out, out_nr, in, in_nr));
}

} // namespace

extern "C" {

// --- sessions ------------------------------------------------------------

int vaccel_sess_init(struct vaccel_session *sess, uint32_t flags)
{
    (void)flags;   // the host's session has no options to carry yet
    if (!sess) {
        return VACCEL_EINVAL;
    }
    auto *dev = virtio::accel::instance();
    if (!dev) {
        return VACCEL_ENODEV;
    }

    uint32_t id = 0;
    int rc = dev->create_session(&id);
    if (rc < 0) {
        return to_vaccel(rc);
    }
    sess->session_id = id;
    sess->resources = nullptr;
    sess->priv = nullptr;
    return VACCEL_OK;
}

int vaccel_sess_free(struct vaccel_session *sess)
{
    if (!sess) {
        return VACCEL_EINVAL;
    }
    auto *dev = virtio::accel::instance();
    if (!dev) {
        return VACCEL_ENODEV;
    }
    return to_vaccel(dev->destroy_session(sess->session_id));
}

// --- matmul --------------------------------------------------------------

int vaccel_matmul_create(struct vaccel_session *sess, vaccel_matmul_ctx *ctx,
                         vaccel_matmul_info *info,
                         vaccel_matmul_io_attr *io_attr)
{
    if (!ctx || !info || !io_attr) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_MATMUL_CREATE;
    const arg out[] = {
        {&type, sizeof(type)},
        {info, sizeof(*info)},
    };
    const arg in[] = {
        {ctx, sizeof(*ctx)},
        {io_attr, sizeof(*io_attr)},
    };
    return op(sess, out, 2, in, 2);
}

int vaccel_matmul_destroy(struct vaccel_session *sess, vaccel_matmul_ctx ctx)
{
    vaccel_op_type type = VACCEL_MATMUL_DESTROY;
    const arg out[] = {
        {&type, sizeof(type)},
        {&ctx, sizeof(ctx)},
    };
    return op(sess, out, 2, nullptr, 0);
}

int vaccel_create_mem(struct vaccel_session *sess, vaccel_matmul_ctx ctx,
                      uint32_t size, vaccel_tensor_mem *result)
{
    if (!result) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_CREATE_MEM;
    const arg out[] = {
        {&type, sizeof(type)},
        {&ctx, sizeof(ctx)},
        {&size, sizeof(size)},
    };
    const arg in[] = {
        {result, sizeof(*result)},
    };
    return op(sess, out, 3, in, 1);
}

int vaccel_destroy_mem(struct vaccel_session *sess, vaccel_matmul_ctx ctx,
                       vaccel_tensor_mem *mem)
{
    if (!mem) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_DESTROY_MEM;
    const arg out[] = {
        {&type, sizeof(type)},
        {&ctx, sizeof(ctx)},
        {mem, sizeof(*mem)},
    };
    return op(sess, out, 3, nullptr, 0);
}

int vaccel_matmul_set_io_mem(struct vaccel_session *sess, vaccel_matmul_ctx ctx,
                             vaccel_tensor_mem_handle *mem,
                             vaccel_matmul_tensor_attr *attr)
{
    if (!attr) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_MATMUL_SET_IO;
    const arg out[] = {
        {&type, sizeof(type)},
        {&ctx, sizeof(ctx)},
        {&mem, sizeof(mem)},          // the handle itself, not its contents
        {attr, sizeof(*attr)},
    };
    return op(sess, out, 4, nullptr, 0);
}

int vaccel_matmul_set_core_mask(struct vaccel_session *sess,
                                vaccel_matmul_ctx ctx,
                                vaccel_core_mask core_mask)
{
    vaccel_op_type type = VACCEL_MATMUL_SET_CORE_MASK;
    const arg out[] = {
        {&type, sizeof(type)},
        {&ctx, sizeof(ctx)},
        {&core_mask, sizeof(core_mask)},
    };
    return op(sess, out, 3, nullptr, 0);
}

int vaccel_matmul_run(struct vaccel_session *sess, vaccel_matmul_ctx ctx)
{
    vaccel_op_type type = VACCEL_MATMUL_RUN;
    const arg out[] = {
        {&type, sizeof(type)},
        {&ctx, sizeof(ctx)},
    };
    return op(sess, out, 2, nullptr, 0);
}

int vaccel_matmul_set_matrix(struct vaccel_session *sess,
                             vaccel_tensor_mem_handle *dst, void *src,
                             size_t nbytes)
{
    if (!src) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_MATMUL_SET_MATRIX;
    const arg out[] = {
        {&type, sizeof(type)},
        {&dst, sizeof(dst)},
        {src, (uint32_t)nbytes},
        {&nbytes, sizeof(nbytes)},
    };
    return op(sess, out, 4, nullptr, 0);
}

int vaccel_matmul_get_matrix(struct vaccel_session *sess, void *dst,
                             vaccel_tensor_mem_handle *src, size_t nbytes)
{
    if (!dst) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_MATMUL_GET_MATRIX;
    const arg out[] = {
        {&type, sizeof(type)},
        {&src, sizeof(src)},
        {&nbytes, sizeof(nbytes)},
    };
    const arg in[] = {
        {dst, (uint32_t)nbytes},
    };
    return op(sess, out, 3, in, 1);
}

int vaccel_matmul_get_props(struct vaccel_session *sess, char *props,
                            size_t nbytes)
{
    if (!props) {
        return VACCEL_EINVAL;
    }
    vaccel_op_type type = VACCEL_MATMUL_GET_PROPS;
    const arg out[] = {
        {&type, sizeof(type)},
        {&nbytes, sizeof(nbytes)},
    };
    const arg in[] = {
        {props, (uint32_t)nbytes},
    };
    return op(sess, out, 2, in, 1);
}

} // extern "C"
