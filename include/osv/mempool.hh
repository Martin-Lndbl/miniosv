/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEMPOOL_HH
#define MEMPOOL_HH

#include <functional>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/list.hpp>
#include <osv/mutex.h>
#include <arch.hh>
#include <osv/pagealloc.hh>
#include <osv/percpu.hh>
#include <osv/condvar.h>
#include <osv/semaphore.hh>
#include <osv/mmu.hh>
#include <osv/contiguous_alloc.hh>
#include <osv/kernel_config.h>

namespace memory {

const size_t page_size = 4096;

void setup_free_memory(void* start, size_t bytes);

namespace bi = boost::intrusive;

struct page_range {
    explicit page_range(size_t size);
    bool operator<(const page_range& pr) const {
        return size < pr.size;
    }
    size_t size;
    boost::intrusive::set_member_hook<> set_hook;
    boost::intrusive::list_member_hook<> list_hook;
};

void free_initial_memory_range(void* addr, size_t size);

// Print the allocation-size histogram gathered under conf_memory_histogram.
// A no-op when it is off.
void histogram_dump();

extern bool tracker_enabled;

}

#endif
