/*
 * Objects allocated before the heap exists.
 *
 * A bump pointer through one page at a time, with the size of every object in
 * the two bytes before it and a count of them in the page header. A page goes
 * back when its count reaches zero, and the last object allocated can be
 * un-bumped, which is enough to keep boot from walking through memory: nothing
 * else here reuses anything.
 *
 * It has to work before there are threads, before there is a scheduler, and
 * before the frame allocator has taken over from the boot regions, which is
 * why it is this and not the heap.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <cassert>
#include <cstdint>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/early.hh>
#include <osv/mem/frames.hh>
#include <osv/mutex.h>
#include "linear.hh"

namespace mem {
namespace early {

namespace {

constexpr size_t page_size = frames::page_size;

// The count is what lets a page go back: every object in it took one, and the
// page is done when they have all been given back.
struct page_header {
    unsigned short allocations_count;
};

mutex lock;

// The page the bump pointer is in, and where free memory starts within it.
// "next" is the first byte after the object allocated last, not the address
// the next one will get: that has to account for alignment and for the two
// bytes of size in front of it.
char *page = nullptr;
size_t next_offset = 0;
size_t previous_offset = 0;

// Pages that this allocator holds
constexpr unsigned max_pages = 64;
page_header *held[max_pages];
unsigned held_count;

page_header *header_of(void *object)
{
    return reinterpret_cast<page_header *>(
            reinterpret_cast<std::uintptr_t>(object) & ~(page_size - 1));
}

unsigned short *size_field(void *object)
{
    return reinterpret_cast<unsigned short *>(
            static_cast<char *>(object) - sizeof(unsigned short));
}

// Not the boot allocator: by the time this needs a page, llfree may already
// own the memory. frames::alloc() picks whichever is current.
void take_page()
{
    if (held_count == max_pages) {
        abort("early: more than %u pages of small objects are live before the "
              "heap exists.\n       Raise max_pages in core/mem/early.cc.\n",
              max_pages);
    }
    page = static_cast<char *>(frames::to_linear(frames::alloc()));
    header_of(page)->allocations_count = 0;
    next_offset = sizeof(page_header);
    held[held_count++] = header_of(page);
}

void give_back(page_header *h)
{
    for (unsigned i = 0; i < held_count; i++) {
        if (held[i] == h) {
            held[i] = held[--held_count];
            break;
        }
    }
    frames::free(frames::from_linear(h));
}

}

bool takes(size_t bytes, size_t alignment)
{
    auto lowest = align_up(sizeof(page_header) + sizeof(unsigned short), alignment);
    return lowest + bytes <= page_size;
}

void *alloc(size_t bytes, size_t alignment)
{
    WITH_LOCK(lock) {
        if (!page) {
            take_page();
        }

        size_t offset = align_up(next_offset + sizeof(unsigned short), alignment);
        if (offset + bytes > page_size) {
            take_page();
            offset = align_up(next_offset + sizeof(unsigned short), alignment);
        }
        assert(offset + bytes <= page_size);

        auto ret = page + offset;
        previous_offset = next_offset;
        next_offset = offset + bytes;

        *size_field(ret) = static_cast<unsigned short>(bytes);
        header_of(page)->allocations_count++;
        return ret;
    }
}

void free(void *p)
{
    WITH_LOCK(lock) {
        page_header *h = header_of(p);
        unsigned short bytes = *size_field(p);
        if (!bytes) {
            return;
        }
        // Zero says it has been freed, so a second free does nothing.
        *size_field(p) = 0;

        if (--h->allocations_count == 0) {
            give_back(h);
            if (page == reinterpret_cast<char *>(h)) {
                page = nullptr;
            }
        } else if (page == reinterpret_cast<char *>(h) &&
                   page + (next_offset - bytes) == p) {
            // The object allocated last, freed straight after: put the bump
            // pointer back where it was. This is what makes a malloc/free pair
            // during boot cost nothing.
            next_offset = previous_offset;
        }
    }
}

size_t size_of(void *p)
{
    return *size_field(p);
}

bool owns(void *p)
{
    page_header *h = header_of(p);
    WITH_LOCK(lock) {
        for (unsigned i = 0; i < held_count; i++) {
            if (held[i] == h) {
                return true;
            }
        }
    }
    return false;
}

}
}
