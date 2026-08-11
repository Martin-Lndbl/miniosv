/*
 * virtio-accel transport. See drivers/virtio-accel.hh.
 *
 * The descriptor layout is fixed by the device (virtio_accel_handle_request in
 * the QEMU subproject), and it is stricter than "some out buffers, some in
 * buffers":
 *
 *   out (device-readable), in this order
 *     1. the header, alone in its descriptor -- the device consumes exactly
 *        sizeof(hdr) and then takes the next descriptor's base address
 *     2. the out argument array, one contiguous descriptor, if out_nr > 0
 *     3. the in argument array, one contiguous descriptor, if in_nr > 0
 *        (device-readable: the device needs the lengths, not the data)
 *     4. each out argument's data, one descriptor each
 *
 *   in (device-writable), in this order
 *     5. each in argument's data, one descriptor each
 *     6. for CREATE_SESSION, the session id
 *     7. the status word, last, and exactly 4 bytes -- the device checks this
 *
 * Everything is staged through one physically contiguous allocation. The
 * caller's buffers come from wherever the application allocates (ggml uses
 * mmap), and a vring descriptor has to name a physical address, so they would
 * otherwise have to be linearly mapped. Copying also keeps the argument arrays
 * contiguous, which the device requires.
 */

#include "drivers/virtio-accel.hh"

#include <cerrno>
#include <cstring>
#include <vector>

#include <osv/contiguous_alloc.hh>
#include <cstdio>

#include <osv/debug.hh>
#include <osv/mmu.hh>
#include <osv/sched.hh>

#include "drivers/virtio-device.hh"

namespace virtio {

accel *accel::_instance = nullptr;

namespace {

// Every descriptor starts here, so keep them apart by the widest alignment the
// structures need. The device reads the argument arrays as packed structs.
const size_t ARG_ALIGN = 16;

size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

// A staging area for one request: physically contiguous, so each piece can be
// handed to the ring by address.
class staging {
public:
    explicit staging(size_t size)
        : _size(size)
        , _p(static_cast<uint8_t *>(
              memory::alloc_phys_contiguous_aligned(size, ARG_ALIGN)))
    {
        if (_p) {
            memset(_p, 0, size);
        }
    }
    ~staging()
    {
        if (_p) {
            memory::free_phys_contiguous_aligned(_p);
        }
    }
    staging(const staging &) = delete;
    staging &operator=(const staging &) = delete;

    explicit operator bool() const { return _p != nullptr; }

    //! Carve `len` bytes off the front, aligned. Returns nullptr if the
    //! reservation was short, which would be a bug in the size calculation.
    uint8_t *take(size_t len)
    {
        const size_t at = align_up(_used, ARG_ALIGN);
        if (at + len > _size) {
            return nullptr;
        }
        _used = at + len;
        return _p + at;
    }

private:
    size_t _size;
    size_t _used = 0;
    uint8_t *_p;
};

} // namespace

accel::accel(virtio_device &dev)
    : virtio_driver(dev)
{
    setup_features();
    probe_virt_queues();

    _queue = get_virt_queue(0);

    interrupt_factory int_factory;
    int_factory.register_msi_bindings = [this](interrupt_manager &msi) {
        // No bottom-half thread: the caller waits for its own request, so the
        // ISR wakes it directly rather than handing off to a worker.
        msi.easy_register({{0, [this] {
                                this->_queue->disable_interrupts();
                                auto *w = this->_waiter.load(std::memory_order_acquire);
                                if (w) {
                                    w->wake_with_irq_disabled();
                                }
                            },
                            nullptr}});
    };
    _dev.register_interrupt(int_factory);

    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);

    _instance = this;
    // printf, not debug(): debug() only reaches the console when the global
    // verbose flag is set, and the other drivers here announce themselves.
    printf("virtio-accel: attached, one queue of %d\n",
           (int)_queue->size());
}

accel::~accel()
{
    if (_instance == this) {
        _instance = nullptr;
    }
}

hw_driver *accel::probe(hw_device *dev)
{
    return virtio::probe<accel, VIRTIO_ID_ACCEL>(dev);
}

int accel::create_session(uint32_t *sess_id)
{
    if (!sess_id) {
        return -EINVAL;
    }
    return request(ACCEL_CREATE_SESSION, 0, nullptr, 0, nullptr, 0, sess_id);
}

int accel::destroy_session(uint32_t sess_id)
{
    return request(ACCEL_DESTROY_SESSION, sess_id, nullptr, 0, nullptr, 0,
                   nullptr);
}

int accel::do_op(uint32_t sess_id, const arg *out, uint32_t out_nr,
                 const arg *in, uint32_t in_nr)
{
    return request(ACCEL_DO_OP, sess_id, out, out_nr, in, in_nr, nullptr);
}

int accel::request(uint32_t op_type, uint32_t sess_id,
                   const arg *out, uint32_t out_nr,
                   const arg *in, uint32_t in_nr,
                   uint32_t *sess_id_out)
{
    const bool want_sess_id = (sess_id_out != nullptr);

    // Size the staging area: header, the two argument arrays, every argument's
    // data, the session id and the status, each aligned.
    size_t need = align_up(sizeof(accel_wire_hdr), ARG_ALIGN);
    if (out_nr) {
        need += align_up(out_nr * sizeof(accel_wire_arg), ARG_ALIGN);
    }
    if (in_nr) {
        need += align_up(in_nr * sizeof(accel_wire_arg), ARG_ALIGN);
    }
    for (uint32_t i = 0; i < out_nr; i++) {
        need += align_up(out[i].len, ARG_ALIGN);
    }
    for (uint32_t i = 0; i < in_nr; i++) {
        need += align_up(in[i].len, ARG_ALIGN);
    }
    // The device writes the session id with a 64-bit store even though the
    // value is 32 bits, so reserve eight bytes for it rather than let it run
    // into whatever follows.
    need += align_up(sizeof(uint64_t), ARG_ALIGN);
    need += align_up(sizeof(uint32_t), ARG_ALIGN);

    staging buf(need);
    if (!buf) {
        return -ENOMEM;
    }

    auto *hdr = reinterpret_cast<accel_wire_hdr *>(
        buf.take(sizeof(accel_wire_hdr)));
    accel_wire_arg *out_args = nullptr;
    accel_wire_arg *in_args = nullptr;
    if (out_nr) {
        out_args = reinterpret_cast<accel_wire_arg *>(
            buf.take(out_nr * sizeof(accel_wire_arg)));
    }
    if (in_nr) {
        in_args = reinterpret_cast<accel_wire_arg *>(
            buf.take(in_nr * sizeof(accel_wire_arg)));
    }

    std::vector<uint8_t *> out_data(out_nr);
    std::vector<uint8_t *> in_data(in_nr);
    for (uint32_t i = 0; i < out_nr; i++) {
        out_data[i] = buf.take(out[i].len);
        out_args[i].len = out[i].len;
        memcpy(out_data[i], out[i].buf, out[i].len);
    }
    for (uint32_t i = 0; i < in_nr; i++) {
        in_data[i] = buf.take(in[i].len);
        in_args[i].len = in[i].len;
    }

    auto *sid = reinterpret_cast<uint64_t *>(buf.take(sizeof(uint64_t)));
    auto *status = reinterpret_cast<uint32_t *>(buf.take(sizeof(uint32_t)));
    if (!hdr || !sid || !status) {
        return -ENOMEM;  // the reservation above was wrong
    }

    hdr->sess_id = sess_id;
    hdr->op_type = op_type;
    hdr->op.out_nr = out_nr;
    hdr->op.in_nr = in_nr;
    hdr->op.out = out_args;
    hdr->op.in = in_args;
    *status = ACCEL_S_ERR;

    WITH_LOCK(_lock) {
        _queue->init_sg();

        _queue->add_out_sg(hdr, sizeof(*hdr));
        if (out_nr) {
            _queue->add_out_sg(out_args, out_nr * sizeof(accel_wire_arg));
        }
        if (in_nr) {
            _queue->add_out_sg(in_args, in_nr * sizeof(accel_wire_arg));
        }
        for (uint32_t i = 0; i < out_nr; i++) {
            _queue->add_out_sg(out_data[i], out[i].len);
        }
        for (uint32_t i = 0; i < in_nr; i++) {
            _queue->add_in_sg(in_data[i], in[i].len);
        }
        if (want_sess_id) {
            _queue->add_in_sg(sid, sizeof(*sid));
        }
        _queue->add_in_sg(status, sizeof(*status));

        if (!_queue->add_buf(hdr)) {
            return -ENOSPC;
        }
        _waiter.store(sched::thread::current(), std::memory_order_release);
        _queue->kick();

        wait_for_queue(_queue, &vring::used_ring_not_empty);
        _waiter.store(nullptr, std::memory_order_release);

        u32 len = 0;
        _queue->get_buf_elem(&len);
        _queue->get_buf_finalize();
    }

    if (*status != ACCEL_S_OK) {
        printf("virtio-accel: op %d failed with status %d\n", (int)op_type,
               (int)*status);
        return -EIO;
    }

    // Results land in the staging area; hand them back to the caller.
    for (uint32_t i = 0; i < in_nr; i++) {
        memcpy(in[i].buf, in_data[i], in[i].len);
    }
    if (want_sess_id) {
        *sess_id_out = static_cast<uint32_t>(*sid);
    }
    return 0;
}

void accel::handle_irq()
{
}

bool accel::ack_irq()
{
    return _dev.read_and_ack_isr();
}

}
