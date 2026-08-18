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

extern size_t phys_mem_size;

void setup_free_memory(void* start, size_t bytes);

namespace bi = boost::intrusive;

// Please note that early_page_header and pool:page_header
// structs have common 'owner' field. The owner field
// in early_page_header is always set to 'nullptr' and allows
// us to differentiate between pages used by early
// malloc and regular malloc pools
struct early_page_header {
    void* owner;
    unsigned short allocations_count;
};

struct free_object {
    free_object* next;
};

class pool {
public:
    explicit pool(unsigned size);
    ~pool();
    void* alloc();
    void free(void* object);
    unsigned get_size();
    static pool* from_object(void* object);
    static void collect_garbage();
private:
    struct page_header;
private:
    bool have_full_pages();
    void add_page();
    static page_header* to_header(free_object* object);

    // should get called with the preemption lock taken
    void free_same_cpu(free_object* obj, unsigned cpu_id);
    void free_different_cpu(free_object* obj, unsigned obj_cpu, unsigned cur_cpu);
private:
    unsigned _size;

    struct page_header {
        pool* owner;
        unsigned cpu_id;
        unsigned nalloc;
        bi::list_member_hook<> free_link;
        free_object* local_free;  // free objects in this page
    };

    typedef bi::list<page_header,
                     bi::member_hook<page_header,
                                     bi::list_member_hook<>,
                                     &page_header::free_link>,
                     bi::constant_time_size<false>
                    > free_list_base_type;
    class free_list_type : public free_list_base_type {
    public:
        ~free_list_type() { assert(empty()); }
    };
    // maintain a list of free pages percpu
    dynamic_percpu<free_list_type> _free;
public:
    static const size_t max_object_size;
    static const size_t min_object_size;
};

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

extern bool tracker_enabled;





}

#endif
