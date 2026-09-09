// This demo exemplifies hardware performance counter measurements on a single
// core.

#include <osv/power.hh>
#include <osv/perf.hh>

constexpr unsigned iterations{1u << 20};

extern "C" void osv_app_main() {
  // Use default counters
  perf::PerfEvent e{true};
  // Add additional counter (LLC accesses)
  e.registerCounter(perf::PERF_COUNT_HW::STALL_FRONTEND);
  // Start measuring
  e.startCounters();
  for (volatile unsigned i{0}; i < iterations; i += 1)
    ;
  // Stop measuring
  e.stopCounters();
  // Print results with scale factor = 1
  e.printReport(std::cout, 1);

  osv::poweroff();
}
