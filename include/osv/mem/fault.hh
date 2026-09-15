/*
 * Fault handling.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_FAULT_HH
#define OSV_MEM_FAULT_HH

#include <osv/mem/types.hh>

struct exception_frame;

namespace mem {

// Hand a fault to whichever region owns the address, or raise SIGSEGV.
void vm_fault(uintptr_t addr, exception_frame *ef);

// What demand paging has cost. The other half of the memory bill, next to
// mem::mapping::tlb_shootdown_stats(): a fault is a region lookup plus
// whatever that region's handler does, and a workload that touches a lot of
// freshly mapped memory pays one per page it has not seen before. `ns_total`
// is wall time inside vm_fault summed over cpus, so it is comparable to the
// thread-time a query spends outside its I/O.
//
// Timing costs two clock reads per fault. That is deliberate and it is not
// free -- if faults turn out to be the hot path, this measurement is itself
// on it, and the count is the number to trust over the total.
struct fault_stats {
    uint64_t count;
    uint64_t sigsegv;
    uint64_t ns_total;
    uint64_t ns_max;
};

fault_stats vm_fault_stats();

}

#endif /* OSV_MEM_FAULT_HH */
