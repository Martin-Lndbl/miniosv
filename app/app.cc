// This is a minimal "hello world" placeholder.

#include <cstdint>
#include <osv/power.hh>
#include <osv/perf.hh>

extern "C" void osv_app_main()
{
    printf("Hello, world from OSv!\n");
    while (true) {
        asm volatile("" ::: "memory");
    }
}
