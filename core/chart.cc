#include "arch.hh"
#include <osv/debug.hh>
#include <osv/sched.hh>
#include "drivers/clock.hh"
#include <osv/boot.hh>

double boot_time_chart::to_msec(u64 time)
{
    return (double)clock::get()->processor_to_nano(time) / 1000000;
}

void boot_time_chart::print_one_time(int index)
{
    if (!arrays[index].str) {
        return;
    }
    auto field = arrays[index].stamp;
    auto last = arrays[index - 1].stamp;
    auto initial = arrays[0].stamp;
    printf("\t%s: %.2fms, (+%.2fms)\n", arrays[index].str, to_msec(field - initial), to_msec(field - last));
}

#ifdef __aarch64__
// Mirror every chart event to the host: hvc #0x42 with an event id in x0 is
// recorded by the kvm_hvc_arm64 tracepoint (see benchmarks/boottime in
// lros-expe; 260 offsets past the ids that script assigns to OVMF and Linux).
// KVM answers the unknown HVC with SMCCC "not supported" in x0; the guest
// just continues. miniosv records all chart events live (the saved-stamp path
// of the original OSv is unused), so the marker fires at event time.
static void trace_event_to_host(int event_idx)
{
    register unsigned long x0 __asm__("x0") = 260 + event_idx;
    __asm__ __volatile__("hvc %1" : "+r"(x0) : "i"(0x42) : "memory");
}
#else
static void trace_event_to_host(int) {}
#endif

void boot_time_chart::event(const char *str)
{
    event(_event++, str, processor::ticks());
}

void boot_time_chart::event(int event_idx, const char *str)
{
    event(event_idx, str, processor::ticks());
}

void boot_time_chart::event(int event_idx, const char *str, u64 stamp)
{
    arrays[event_idx].str = str;
    arrays[event_idx].stamp = stamp;
    trace_event_to_host(event_idx);
}

void boot_time_chart::print_chart()
{
    if (clock::get()->processor_to_nano(10000) == 0) {
        debug("Skipping bootchart: please run this with a clocksource that can do ticks/nanoseconds conversion.\n");
        return;
    }
    int events = _event;
    for (auto i = 1; i < events; ++i) {
        print_one_time(i);
    }
}

void boot_time_chart::print_total_time()
{
    auto last = arrays[_event - 1].stamp;
    auto initial = arrays[0].stamp;
    printf("Booted up in %.2f ms\n", to_msec(last - initial));
}
