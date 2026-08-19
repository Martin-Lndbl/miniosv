/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mmu.hh>
#include <osv/mem/vspace.hh>
#include <osv/mempool.hh>
#include "processor.hh"
#include "exceptions.hh"
#include "dump.hh"
#include "libc/signal.hh"
#include <osv/debug.hh>
#include <string.h>
#include <assert.h>
#include <osv/align.hh>
#include <osv/ilog2.hh>
#include <safe-ptr.hh>
#include <osv/error.h>
#include <osv/trace.hh>
#include <osv/rcu.hh>
#include <algorithm>

#include <osv/kernel_config.h>

// FIXME: Without this pragma, we get a lot of warnings that I don't know
// how to explain or fix. For now, let's just ignore them :-(
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

extern void* elf_start;
extern size_t elf_size;

extern const char text_start[], text_end[];

namespace mmu {

// 1's for the bits provided by the pte for this level
// 0's for the bits provided by the virtual address for this level
phys pte_level_mask(unsigned level)
{
    auto shift = level * ilog2_roundup_constexpr(pte_per_page)
        + ilog2_roundup_constexpr(page_size);
    return ~((phys(1) << shift) - 1);
}

// Physical base of the kernel ELF image and the runtime virtual->physical shift
// for it. Both are set during early arch setup from where the UEFI stub loaded
// the kernel (which is no longer a fixed physical address on either arch).
void *elf_phys_start;
extern "C" u64 kernel_vm_shift;

void* phys_to_virt(phys pa)
{
    void* phys_addr = reinterpret_cast<void*>(pa);
    if ((phys_addr >= elf_phys_start) && (phys_addr < static_cast<char*>(elf_phys_start) + elf_size)) {
        return static_cast<char*>(phys_addr) + kernel_vm_shift;
    }

    return phys_mem + pa;
}

phys virt_to_phys_pt(void* virt);

phys virt_to_phys(void *virt)
{
    if ((virt >= elf_start) && (virt < static_cast<char*>(elf_start) + elf_size)) {
        return reinterpret_cast<phys>(static_cast<char*>(virt) - kernel_vm_shift);
    }

    // For now, only allow non-mmaped areas.  Later, we can either
    // bounce such addresses, or lock them in memory and translate
    assert(virt >= phys_mem);
    return reinterpret_cast<uintptr_t>(virt) & (mem_area_size - 1);
}

template <int N, typename MakePTE>
phys allocate_intermediate_level(MakePTE make_pte)
{
    phys pt_page = virt_to_phys(memory::alloc_page());
    // since the pt is not yet mapped, we don't need to use hw_ptep
    pt_element<N>* pt = phys_cast<pt_element<N>>(pt_page);
    for (auto i = 0; i < pte_per_page; ++i) {
        pt[i] = make_pte(i);
    }
    return pt_page;
}

template<int N>
void allocate_intermediate_level(hw_ptep<N> ptep, pt_element<N> org)
{
    phys pt_page = allocate_intermediate_level<N>([org](int i) {
        auto tmp = org;
        phys addend = phys(i) << page_size_shift;
        tmp.set_addr(tmp.addr() | addend, false);
        return tmp;
    });
    ptep.write(make_intermediate_pte(ptep, pt_page));
}

template<int N>
void allocate_intermediate_level(hw_ptep<N> ptep)
{
    phys pt_page = allocate_intermediate_level<N>([](int i) {
        return make_empty_pte<N>();
    });
    if (!ptep.compare_exchange(make_empty_pte<N>(), make_intermediate_pte(ptep, pt_page))) {
        memory::free_page(phys_to_virt(pt_page));
    }
}

// only 4k can be cow for now
template<int N>
bool change_perm(hw_ptep<N> ptep, unsigned int perm)
{
    static_assert(pt_level_traits<N>::leaf_capable::value, "non leaf pte");
    pt_element<N> pte = ptep.read();
    unsigned int old = (pte.valid() ? perm_read : 0) |
        (pte.writable() ? perm_write : 0) |
        (pte.executable() ? perm_exec : 0);

    // Note: in x86, if the present bit (0x1) is off, not only read is
    // disallowed, but also write and exec. So in mprotect, if any
    // permission is requested, we must also grant read permission.
    // Linux does this too.
    pte.set_valid(true);
    pte.set_writable(perm & perm_write);
    pte.set_executable(perm & perm_exec);
    pte.set_rsvd_bit(0, !perm);
    ptep.write(pte);

#ifdef __x86_64__
    return old & ~perm;
#endif
#ifdef __aarch64__
    //TODO: This will trigger full tlb flush in slightly more cases than on x64
    //and in future we should investigate more precise and hopefully lighter
    //mechanism. But for now it will do it.
    return old != perm;
#endif
}

template<int N>
void split_large_page(hw_ptep<N> ptep)
{
}

template<>
void split_large_page(hw_ptep<1> ptep)
{
    pt_element<1> pte_orig = ptep.read();
    pte_orig.set_large(false);
    allocate_intermediate_level(ptep, pte_orig);
}

struct page_allocator {
    virtual bool map(uintptr_t offset, hw_ptep<0> ptep, pt_element<0> pte, bool write) = 0;
    virtual bool map(uintptr_t offset, hw_ptep<1> ptep, pt_element<1> pte, bool write) = 0;
    virtual bool unmap(void *addr, uintptr_t offset, hw_ptep<0> ptep) = 0;
    virtual bool unmap(void *addr, uintptr_t offset, hw_ptep<1> ptep) = 0;
    virtual ~page_allocator() {}
};

void clamp(uintptr_t& vstart1, uintptr_t& vend1,
           uintptr_t min, size_t max, size_t slop)
{
    vstart1 &= ~(slop - 1);
    vend1 |= (slop - 1);
    vstart1 = std::max(vstart1, min);
    vend1 = std::min(vend1, max);
}

inline unsigned pt_index(uintptr_t virt, unsigned level)
{
    return pt_index(reinterpret_cast<void*>(virt), level);
}

unsigned nr_page_sizes = 2; // FIXME: detect 1GB pages

enum class allocate_intermediate_opt : bool {no = true, yes = false};
enum class skip_empty_opt : bool {no = true, yes = false};
enum class descend_opt : bool {no = true, yes = false};
enum class once_opt : bool {no = true, yes = false};
enum class split_opt : bool {no = true, yes = false};
enum class account_opt: bool {no = true, yes = false};

// Parameter descriptions:
//  Allocate - if "yes" page walker will allocate intermediate page if one is missing
//             otherwise it will skip to next address.
//  Skip     - if "yes" page walker will not call leaf page handler on an empty pte.
//  Descend  - if "yes" page walker will descend one level if large page range is mapped
//             by small pages, otherwise it will call huge_page() on intermediate small pte
//  Once     - if "yes" page walker will not loop over range of pages
//  Split    - If "yes" page walker will split huge pages to small pages while walking
template<allocate_intermediate_opt Allocate, skip_empty_opt Skip = skip_empty_opt::yes,
        descend_opt Descend = descend_opt::yes, once_opt Once = once_opt::no, split_opt Split = split_opt::yes>
class page_table_operation {
protected:
    template<typename T>  bool opt2bool(T v) { return v == T::yes; }
public:
    bool allocate_intermediate(void) { return opt2bool(Allocate); }
    bool skip_empty(void) { return opt2bool(Skip); }
    bool descend(void) { return opt2bool(Descend); }
    bool once(void) { return opt2bool(Once); }
    template<int N>
    bool split_large(hw_ptep<N> ptep, int level) { return opt2bool(Split); }
    unsigned nr_page_sizes(void) { return mmu::nr_page_sizes; }

    template<int N>
    pt_element<N> ptep_read(hw_ptep<N> ptep) { return ptep.read(); }

    // page() function is called on leaf ptes. Each page table operation
    // have to provide its own version.
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) { assert(0); }
    // if huge page range is covered by smaller pages some page table operations
    // may want to have special handling for level 1 non leaf pte. intermediate_page_pre()
    // is called just before descending into the next level, while intermediate_page_post()
    // is called just after.
    void intermediate_page_pre(hw_ptep<1> ptep, uintptr_t offset) {}
    void intermediate_page_post(hw_ptep<1> ptep, uintptr_t offset) {}
    // Page walker calls page() when it a whole leaf page need to be handled, but if it
    // has 2M pte and less then 2M of virt memory to operate upon and split is disabled
    // sup_page is called instead. So if you are here it means that page walker encountered
    // 2M pte and page table operation wants to do something special with sub-region of it
    // since it disabled splitting.
    void sub_page(hw_ptep<1> ptep, int level, uintptr_t offset) { return; }
};

template<typename PageOps, int N>
static inline typename std::enable_if<pt_level_traits<N>::large_capable::value>::type
sub_page(PageOps& pops, hw_ptep<N> ptep, int level, uintptr_t offset)
{
    pops.sub_page(ptep, level, offset);
}

template<typename PageOps, int N>
static inline typename std::enable_if<!pt_level_traits<N>::large_capable::value>::type
sub_page(PageOps& pops, hw_ptep<N> ptep, int level, uintptr_t offset)
{
}

template<typename PageOps, int N>
static inline typename std::enable_if<pt_level_traits<N>::leaf_capable::value, bool>::type
page(PageOps& pops, hw_ptep<N> ptep, uintptr_t offset)
{
    return pops.page(ptep, offset);
}

template<typename PageOps, int N>
static inline typename std::enable_if<!pt_level_traits<N>::leaf_capable::value, bool>::type
page(PageOps& pops, hw_ptep<N> ptep, uintptr_t offset)
{
    assert(0);
    return false;
}

template<typename PageOps, int N>
static inline typename std::enable_if<pt_level_traits<N>::large_capable::value>::type
intermediate_page_pre(PageOps& pops, hw_ptep<N> ptep, uintptr_t offset)
{
    pops.intermediate_page_pre(ptep, offset);
}

template<typename PageOps, int N>
static inline typename std::enable_if<!pt_level_traits<N>::large_capable::value>::type
intermediate_page_pre(PageOps& pops, hw_ptep<N> ptep, uintptr_t offset)
{
}

template<typename PageOps, int N>
static inline typename std::enable_if<pt_level_traits<N>::large_capable::value>::type
intermediate_page_post(PageOps& pops, hw_ptep<N> ptep, uintptr_t offset)
{
    pops.intermediate_page_post(ptep, offset);
}

template<typename PageOps, int N>
static inline typename std::enable_if<!pt_level_traits<N>::large_capable::value>::type
intermediate_page_post(PageOps& pops, hw_ptep<N> ptep, uintptr_t offset)
{
}

template<typename PageOp, int ParentLevel> class map_level;

template<typename PageOp>
        void map_range(uintptr_t vma_start, uintptr_t vstart, size_t size, PageOp& page_mapper, size_t slop = page_size)
{
    map_level<PageOp, 4> pt_mapper(vma_start, vstart, size, page_mapper, slop);
    pt_mapper(hw_ptep<4>::force(mmu::get_root_pt(vstart)));
    // On some architectures with weak memory model it is necessary
    // to force writes to page table entries complete and instruction pipeline
    // flushed so that new mappings are properly visible when relevant newly mapped
    // virtual memory areas are accessed right after this point.
    // So let us call arch-specific function to execute the logic above if
    // applicable for given architecture.
    synchronize_page_table_modifications();
}

template<typename PageOp, int ParentLevel> class map_level {
private:
    uintptr_t vma_start;
    uintptr_t vcur;
    uintptr_t vend;
    size_t slop;
    PageOp& page_mapper;
    static constexpr int level = ParentLevel - 1;

    friend void map_range<PageOp>(uintptr_t, uintptr_t, size_t, PageOp&, size_t);
    friend class map_level<PageOp, ParentLevel + 1>;

    map_level(uintptr_t vma_start, uintptr_t vcur, size_t size, PageOp& page_mapper, size_t slop) :
        vma_start(vma_start), vcur(vcur), vend(vcur + size - 1), slop(slop), page_mapper(page_mapper) {}
    pt_element<ParentLevel> read(const hw_ptep<ParentLevel>& ptep) const {
        return page_mapper.ptep_read(ptep);
    }
    pt_element<level> read(const hw_ptep<level>& ptep) const {
        return page_mapper.ptep_read(ptep);
    }
    hw_ptep<level> follow(hw_ptep<ParentLevel> ptep)
    {
        return hw_ptep<level>::force(phys_cast<pt_element<level>>(read(ptep).next_pt_addr()));
    }
    bool skip_pte(hw_ptep<level> ptep) {
        return page_mapper.skip_empty() && read(ptep).empty();
    }
    bool descend(hw_ptep<level> ptep) {
        return page_mapper.descend() && !read(ptep).empty() && !read(ptep).large();
    }
    template<int N>
    typename std::enable_if<N == 0>::type
    map_range(uintptr_t vcur, size_t size, PageOp& page_mapper, size_t slop,
            hw_ptep<N> ptep, uintptr_t base_virt)
    {
    }
    template<int N>
    typename std::enable_if<N == level && N != 0>::type
    map_range(uintptr_t vcur, size_t size, PageOp& page_mapper, size_t slop,
            hw_ptep<N> ptep, uintptr_t base_virt)
    {
        map_level<PageOp, level> pt_mapper(vma_start, vcur, size, page_mapper, slop);
        pt_mapper(ptep, base_virt);
    }
    void operator()(hw_ptep<ParentLevel> parent, uintptr_t base_virt = 0) {
        if (!read(parent).valid()) {
            if (!page_mapper.allocate_intermediate()) {
                return;
            }
            allocate_intermediate_level(parent);
        } else if (read(parent).large()) {
            if (page_mapper.split_large(parent, ParentLevel)) {
                // We're trying to change a small page out of a huge page (or
                // in the future, potentially also 2 MB page out of a 1 GB),
                // so we need to first split the large page into smaller pages.
                // Our implementation ensures that it is ok to free pieces of a
                // alloc_huge_page() with free_page(), so it is safe to do such a
                // split.
                split_large_page(parent);
            } else {
                // If page_mapper does not want to split, let it handle subpage by itself
                sub_page(page_mapper, parent, ParentLevel, base_virt - vma_start);
                return;
            }
        }
        auto pt = follow(parent);
        phys step = phys(1) << (page_size_shift + level * pte_per_page_shift);
        auto idx = pt_index(vcur, level);
        auto eidx = pt_index(vend, level);
        base_virt += idx * step;
        base_virt = (int64_t(base_virt) << 16) >> 16; // extend 47th bit

        do {
            auto ptep = pt.at(idx);
            uintptr_t vstart1 = vcur, vend1 = vend;
            clamp(vstart1, vend1, base_virt, base_virt + step - 1, slop);
            if (unsigned(level) < page_mapper.nr_page_sizes() && vstart1 == base_virt && vend1 == base_virt + step - 1) {
                uintptr_t offset = base_virt - vma_start;
                if (level) {
                    if (!skip_pte(ptep)) {
                        if (descend(ptep) || !page(page_mapper, ptep, offset)) {
                            intermediate_page_pre(page_mapper, ptep, offset);
                            map_range(vstart1, vend1 - vstart1 + 1, page_mapper, slop, ptep, base_virt);
                            intermediate_page_post(page_mapper, ptep, offset);
                        }
                    }
                } else {
                    if (!skip_pte(ptep)) {
                        page(page_mapper, ptep, offset);
                    }
                }
            } else {
                map_range(vstart1, vend1 - vstart1 + 1, page_mapper, slop, ptep, base_virt);
            }
            base_virt += step;
            ++idx;
        } while(!page_mapper.once() && idx <= eidx);
    }
};

class linear_page_mapper :
        public page_table_operation<allocate_intermediate_opt::yes, skip_empty_opt::no, descend_opt::no> {
    phys start;
    phys end;
    mattr mem_attr;
public:
    linear_page_mapper(phys start, size_t size, mattr mem_attr = mattr_default) :
        start(start), end(start + size), mem_attr(mem_attr) {}
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        phys addr = start + offset;
        assert(addr < end);
        ptep.write(make_leaf_pte(ptep, addr, mmu::perm_rwx, mem_attr));
        return true;
    }
};

template<allocate_intermediate_opt Allocate, skip_empty_opt Skip = skip_empty_opt::yes,
         account_opt Account = account_opt::no>
class vma_operation :
        public page_table_operation<Allocate, Skip, descend_opt::yes, once_opt::no, split_opt::yes> {
public:
    // returns true if tlb flush is needed after address range processing is completed.
    bool tlb_flush_needed(void) { return false; }
    // this function is called at the very end of operate_range(). vma_operation may do
    // whatever cleanup is needed here.
    void finalize(void) { return; }

    ulong account_results(void) { return _total_operated; }
    void account(size_t size) { if (this->opt2bool(Account)) _total_operated += size; }
private:
    // We don't need locking because each walk will create its own instance, so
    // while two instances can operate over the same linear address (therefore
    // all the cmpxcghs), the same instance will go linearly over its duty.
    ulong _total_operated = 0;
};

/*
 * populate() populates the page table with the entries it is (assumed to be)
 * missing to span the given virtual-memory address range, and then pre-fills
 * (using the given fill function) these pages and sets their permissions to
 * the given ones. This is part of the mmap implementation.
 */
template <account_opt T = account_opt::no>
class populate : public vma_operation<allocate_intermediate_opt::yes, skip_empty_opt::no, T> {
private:
    page_allocator* _page_provider;
    unsigned int _perm;
    bool _write;
    bool _map_dirty;
    template<int N>
    bool skip(pt_element<N> pte) {
        if (pte.empty()) {
            return false;
        }
        return !_write || pte.writable();
    }
    template<int N>
    inline pt_element<N> dirty(pt_element<N> pte) {
        pte.set_dirty(_map_dirty || _write);
        return pte;
    }
public:
    populate(page_allocator* pops, unsigned int perm, bool write = false, bool map_dirty = true) :
        _page_provider(pops), _perm(perm), _write(write), _map_dirty(map_dirty) { }
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        auto pte = ptep.read();
        if (skip(pte)) {
            return true;
        }

        pte = dirty(make_leaf_pte(ptep, 0, _perm));

        try {
            if (_page_provider->map(offset, ptep, pte, _write)) {
                this->account(pt_level_traits<N>::size::value);
            }
        } catch(std::exception&) {
            return false;
        }
        return true;
    }
};

template <account_opt Account = account_opt::no>
class populate_small : public populate<Account> {
public:
    populate_small(page_allocator* pops, unsigned int perm, bool write = false, bool map_dirty = true) :
        populate<Account>(pops, perm, write, map_dirty) { }
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        assert(!pt_level_traits<N>::large_capable::value);
        return populate<Account>::page(ptep, offset);
    }
    unsigned nr_page_sizes(void) { return 1; }
};

class splithugepages : public vma_operation<allocate_intermediate_opt::no, skip_empty_opt::yes, account_opt::no> {
public:
    splithugepages() { }
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset)
    {
        assert(!pt_level_traits<N>::large_capable::value);
        return true;
    }
    unsigned nr_page_sizes(void) { return 1; }
};

struct tlb_gather {
    static constexpr size_t max_pages = 20;
    struct tlb_page {
        void* addr;
        size_t size;
    };
    size_t nr_pages = 0;
    tlb_page pages[max_pages];
    bool push(void* addr, size_t size) {
        bool flushed = false;
        if (nr_pages == max_pages) {
            flush();
            flushed = true;
        }
        pages[nr_pages++] = { addr, size };
        return flushed;
    }
    bool flush() {
        if (!nr_pages) {
            return false;
        }
        mmu::flush_tlb_all();
        for (auto i = 0u; i < nr_pages; ++i) {
            auto&& tp = pages[i];
            if (tp.size == page_size) {
                memory::free_page(tp.addr);
            } else {
                memory::free_huge_page(tp.addr, tp.size);
            }
        }
        nr_pages = 0;
        return true;
    }
};

/*
 * Undo the operation of populate(), freeing memory allocated by populate()
 * and marking the pages non-present.
 */
template <account_opt T = account_opt::no>
class unpopulate : public vma_operation<allocate_intermediate_opt::no, skip_empty_opt::yes, T> {
private:
    tlb_gather _tlb_gather;
    page_allocator* _pops;
    bool do_flush = false;
public:
    unpopulate(page_allocator* pops) : _pops(pops) {}
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        void* addr = phys_to_virt(ptep.read().addr());
        size_t size = pt_level_traits<N>::size::value;
        // Note: we free the page even if it is already marked "not present".
        // evacuate() makes sure we are only called for allocated pages, and
        // not-present may only mean mprotect(PROT_NONE).
        if (_pops->unmap(addr, offset, ptep)) {
            do_flush = !_tlb_gather.push(addr, size);
        } else {
            do_flush = true;
        }
        this->account(size);
        return true;
    }
    void intermediate_page_post(hw_ptep<1> ptep, uintptr_t offset) {
        osv::rcu_defer([](void *page) { memory::free_page(page); }, phys_to_virt(ptep.read().addr()));
        ptep.write(make_empty_pte<1>());
    }
    bool tlb_flush_needed(void) {
        return !_tlb_gather.flush() && do_flush;
    }
    void finalize(void) {}
};

class protection : public vma_operation<allocate_intermediate_opt::no, skip_empty_opt::yes> {
private:
    unsigned int perm;
    bool do_flush;
public:
    protection(unsigned int perm) : perm(perm), do_flush(false) { }
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        do_flush |= change_perm(ptep, perm);
        return true;
    }
    bool tlb_flush_needed(void) {return do_flush;}
};

class virt_to_phys_map :
        public page_table_operation<allocate_intermediate_opt::no, skip_empty_opt::yes,
        descend_opt::yes, once_opt::yes, split_opt::no> {
private:
    uintptr_t v;
    phys result;
    static constexpr phys null = ~0ull;
    virt_to_phys_map(uintptr_t v) : v(v), result(null) {}

    phys addr(void) {
        assert(result != null);
        return result;
    }
public:
    friend phys virt_to_phys_pt(void* virt);
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        assert(result == null);
        result = ptep.read().addr() | (v & ~pte_level_mask(N));
        return true;
    }
    void sub_page(hw_ptep<1> ptep, int l, uintptr_t offset) {
        assert(ptep.read().large());
        page(ptep, offset);
    }
};

class virt_to_pte_map_rcu :
        public page_table_operation<allocate_intermediate_opt::no, skip_empty_opt::yes,
        descend_opt::yes, once_opt::yes, split_opt::no> {
private:
    virt_pte_visitor& _visitor;
    virt_to_pte_map_rcu(virt_pte_visitor& visitor) : _visitor(visitor) {}

public:
    friend void virt_visit_pte_rcu(uintptr_t, virt_pte_visitor&);
    template<int N>
    pt_element<N> ptep_read(hw_ptep<N> ptep) {
        return ptep.ll_read();
    }
    template<int N>
    bool page(hw_ptep<N> ptep, uintptr_t offset) {
        auto pte = ptep_read(ptep);
        _visitor.pte(pte);
        assert(pt_level_traits<N>::large_capable::value == pte.large());
        return true;
    }
    void sub_page(hw_ptep<1> ptep, int l, uintptr_t offset) {
        page(ptep, offset);
    }
};

template<typename T> ulong operate_range(T mapper, void *vma_start, void *start, size_t size)
{
    start = align_down(start, page_size);
    size = std::max(align_up(size, page_size), page_size);
    uintptr_t virt = reinterpret_cast<uintptr_t>(start);
    map_range(reinterpret_cast<uintptr_t>(vma_start), virt, size, mapper);

    // TODO: consider if instead of requesting a full TLB flush, we should
    // instead try to make more judicious use of INVLPG - e.g., in
    // split_large_page() and other specific places where we modify specific
    // page table entries.
    if (mapper.tlb_flush_needed()) {
        mmu::flush_tlb_all();
    }
    mapper.finalize();
    return mapper.account_results();
}

template<typename T> ulong operate_range(T mapper, void *start, size_t size)
{
    return operate_range(mapper, start, start, size);
}

phys virt_to_phys_pt(void* virt)
{
    auto v = reinterpret_cast<uintptr_t>(virt);
    auto vbase = align_down(v, page_size);
    virt_to_phys_map v2p_mapper(v);
    map_range(vbase, vbase, page_size, v2p_mapper);
    return v2p_mapper.addr();
}

void virt_visit_pte_rcu(uintptr_t virt, virt_pte_visitor& visitor)
{
    auto vbase = align_down(virt, page_size);
    virt_to_pte_map_rcu v2pte_mapper(visitor);
    WITH_LOCK(osv::rcu_read_lock) {
        map_range(vbase, vbase, page_size, v2pte_mapper);
    }
}

class uninitialized_anonymous_page_provider : public page_allocator {
private:
    virtual void* fill(void* addr, uint64_t offset, uintptr_t size) {
        return addr;
    }
    template<int N>
    bool set_pte(void *addr, hw_ptep<N> ptep, pt_element<N> pte) {
        if (!addr) {
            throw std::exception();
        }
        if (!write_pte(addr, ptep, make_empty_pte<N>(), pte)) {
            if (pt_level_traits<N>::large_capable::value) {
                memory::free_huge_page(addr, pt_level_traits<N>::size::value);
            } else {
                memory::free_page(addr);
            }
            return false;
        }
        return true;
    }
public:
    virtual bool map(uintptr_t offset, hw_ptep<0> ptep, pt_element<0> pte, bool write) override {
        return set_pte(fill(memory::alloc_page(), offset, page_size), ptep, pte);
    }
    virtual bool map(uintptr_t offset, hw_ptep<1> ptep, pt_element<1> pte, bool write) override {
        size_t size = pt_level_traits<1>::size::value;
        return set_pte(fill(memory::alloc_huge_page(size), offset, size), ptep, pte);
    }
    virtual bool unmap(void *addr, uintptr_t offset, hw_ptep<0> ptep) override {
        clear_pte(ptep);
        return true;
    }
    virtual bool unmap(void *addr, uintptr_t offset, hw_ptep<1> ptep) override {
        clear_pte(ptep);
        return true;
    }
};

class initialized_anonymous_page_provider : public uninitialized_anonymous_page_provider {
private:
    virtual void* fill(void* addr, uint64_t offset, uintptr_t size) override {
        if (addr) {
            memset(addr, 0, size);
        }
        return addr;
    }
};

static uninitialized_anonymous_page_provider page_allocator_noinit;
static initialized_anonymous_page_provider page_allocator_init;

static page_allocator *anon_provider(bool zero)
{
    return zero ? static_cast<page_allocator*>(&page_allocator_init)
                : static_cast<page_allocator*>(&page_allocator_noinit);
}

void populate_anon(void *region_start, void *addr, size_t size, unsigned perm,
                   bool write, bool small_pages, bool zero)
{
    page_allocator *pops = anon_provider(zero);
    if (small_pages) {
        operate_range(populate_small<>(pops, perm, write, true), region_start, addr, size);
    } else {
        operate_range(populate<>(pops, perm, write, true), region_start, addr, size);
    }
    // Where the data and instruction caches are separate, code that was just
    // mapped has to be made visible to the instruction side.
    if (perm & perm_exec) {
        synchronize_cpu_caches(addr, size);
    }
}

void depopulate_anon(void *region_start, void *addr, size_t size)
{
    operate_range(unpopulate<>(anon_provider(true)), region_start, addr, size);
}

void protect_pages(void *region_start, void *addr, size_t size, unsigned perm)
{
    operate_range(protection(perm), region_start, addr, size);
}

void use_small_pages(void *region_start, void *addr, size_t size)
{
    operate_range(splithugepages(), region_start, addr, size);
}

// Everything this file reserves. The kind tells a fault which of the two it
// landed in, and the rest is how an anonymous region wants its pages.
struct tracked_region {
    mem::vspace::region r;
    enum { anon, linear } kind;
    bool zero;
    bool small_pages;
};

static tracked_region *tracked_of(mem::vspace::region *r)
{
    return reinterpret_cast<tracked_region*>(r);
}

void* map_anon(const void* addr, size_t size, unsigned flags, unsigned perm)
{
    size = align_up(size, page_size);
    auto *t = new tracked_region();
    t->kind = tracked_region::anon;
    t->zero = !(flags & mmap_uninitialized);
    t->small_pages = flags & mmap_small;
    t->r.perm = perm;

    auto start = reinterpret_cast<uintptr_t>(addr);
    auto result = (flags & mmap_fixed)
        ? mem::vspace::reserve_at(t->r, {start, start + size})
        : mem::vspace::reserve(t->r, size, size >= huge_page_size ? huge_page_size : page_size);
    if (result != mem::vspace::resa_result::success && !(flags & mmap_fixed) &&
        size >= huge_page_size) {
        // Huge alignment is a preference: a mapping without it beats no mapping.
        result = mem::vspace::reserve(t->r, size, page_size);
    }
    if (result != mem::vspace::resa_result::success) {
        delete t;
        throw make_error(ENOMEM);
    }

    void *v = reinterpret_cast<void*>(t->r.span.start);
    if (flags & mmap_populate) {
        populate_anon(v, v, size, perm, false, t->small_pages, t->zero);
    }
    return v;
}

// The region starting exactly here, or null.
static tracked_region *anon_at(const void *addr, size_t size)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    auto *r = mem::vspace::lookup(start);
    if (!r || r->span.start != start || r->span.size() != align_up(size, page_size)) {
        return nullptr;
    }
    auto *t = tracked_of(r);
    return t->kind == tracked_region::anon ? t : nullptr;
}

bool is_linear_mapped(const void *addr, size_t size)
{
    if ((addr >= elf_start) && (static_cast<const char*>(addr) + size <= static_cast<char*>(elf_start) + elf_size)) {
        return true;
    }
    return addr >= phys_mem;
}

// Is every byte of this region reserved in the address space?
bool ismapped(const void *addr, size_t size)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    return mem::vspace::reserved({start, start + size});
}

// Checks if the entire given memory region is readable.
bool isreadable(void *addr, size_t size)
{
    char *end = align_up((char *)addr + size, mmu::page_size);
    char tmp;
    for (char *p = (char *)addr; p < end; p += mmu::page_size) {
        if (!safe_load(p, tmp))
            return false;
    }
    return true;
}

void linear_map(void* _virt, phys addr, size_t size, const char* name,
                size_t slop, mattr mem_attr)
{
    uintptr_t virt = reinterpret_cast<uintptr_t>(_virt);
    slop = std::min(slop, page_size_level(nr_page_sizes - 1));
    assert((virt & (slop - 1)) == (addr & (slop - 1)));
    linear_page_mapper phys_map(addr, size, mem_attr);
    map_range(virt, virt, size, phys_map, slop);

    // Hold the range so nothing else is handed the same addresses. Some of
    // these overlap -- ACPI maps pages that are also part of a reserved range
    // -- and the first reservation is enough to keep them all out.
    auto *t = new tracked_region();
    t->kind = tracked_region::linear;
    t->r.perm = perm_rwx;
    if (mem::vspace::reserve_at(t->r, {virt, virt + size}) !=
        mem::vspace::resa_result::success) {
        delete t;
    }
}

void free_initial_memory_range(uintptr_t addr, size_t size)
{
    if (!size) {
        return;
    }
    // Most of the time the kernel code references memory using
    // virtual addresses. However some allocated system structures
    // like page tables use physical addresses.
    // For that reason we skip the very 1st page of physical memory
    // so that allocated memory areas NEVER map to physical address 0.
    if (!addr) {
        ++addr;
        --size;
    }
    memory::free_initial_memory_range(phys_cast<void>(addr), size);
}

// Permissions live in the page tables; the region records what was asked for.
error mprotect(const void *addr, size_t len, unsigned perm)
{
    len = align_up(len, page_size);
    auto start = reinterpret_cast<uintptr_t>(addr);
    if (!ismapped(addr, len)) {
        return make_error(ENOMEM);
    }
    if (auto *r = mem::vspace::lookup(start)) {
        r->perm = perm;
    }
    protect_pages(const_cast<void*>(addr), const_cast<void*>(addr), len, perm);
    return no_error();
}

error munmap(const void *addr, size_t length)
{
    auto *t = anon_at(addr, length);
    if (!t) {
        return make_error(EINVAL);
    }
    void *v = const_cast<void*>(addr);
    depopulate_anon(v, v, t->r.span.size());
    mem::vspace::release(t->r);
    delete t;
    return no_error();
}

error advise(void* addr, size_t size, int advice)
{
    size = align_up(size, page_size);
    if (!ismapped(addr, size)) {
        return make_error(ENOMEM);
    }
    if (advice == advise_dontneed) {
        depopulate_anon(addr, addr, size);
        return no_error();
    }
    if (advice == advise_nohugepage) {
        use_small_pages(addr, addr, size);
        return no_error();
    }
    return make_error(EINVAL);
}

// There is nowhere to write anonymous memory back to, so this only reports
// whether the range is mapped at all.
error msync(const void* addr, size_t length, int flags)
{
    return ismapped(addr, length) ? no_error() : make_error(ENOMEM);
}

TRACEPOINT(trace_mmu_vm_fault, "addr=%p, error_code=%x", uintptr_t, unsigned int);
TRACEPOINT(trace_mmu_vm_fault_sigsegv, "addr=%p, error_code=%x, %s", uintptr_t, unsigned int, const char*);
TRACEPOINT(trace_mmu_vm_fault_ret, "addr=%p, error_code=%x", uintptr_t, unsigned int);

static void vm_sigsegv(uintptr_t addr, exception_frame* ef)
{
    void *pc = ef->get_pc();
    if (pc >= text_start && pc < text_end) {
        debug_ll("page fault outside application, addr: 0x%016lx\n", addr);
        dump_registers(ef);
        abort();
    }
    osv::handle_mmap_fault(addr, SIGSEGV, ef);
}

static bool permitted(unsigned perm, unsigned error_code)
{
    if (is_page_fault_insn(error_code)) {
        return perm & perm_exec;
    }
    if (is_page_fault_write(error_code)) {
        return perm & perm_write;
    }
    return perm & perm_read;
}

// Anonymous memory is the only thing that faults. The shared guard is what
// keeps the region reserved while its pages are filled in.
void vm_fault(uintptr_t addr, exception_frame* ef)
{
    unsigned error = ef->get_error();
    trace_mmu_vm_fault(addr, error);
    if (fast_sigsegv_check(addr, ef)) {
        vm_sigsegv(addr, ef);
        trace_mmu_vm_fault_sigsegv(addr, error, "fast");
        return;
    }
    addr = align_down(addr, page_size);

    auto *r = mem::vspace::lookup(addr);
    auto *t = r ? tracked_of(r) : nullptr;
    if (!t || t->kind != tracked_region::anon || !permitted(r->perm, error)) {
        vm_sigsegv(addr, ef);
        trace_mmu_vm_fault_sigsegv(addr, error, "slow");
        return;
    }

    size_t size = page_size;
    if (!t->small_pages) {
        uintptr_t huge_start = align_up(r->span.start, huge_page_size);
        uintptr_t huge_end = align_down(r->span.end, huge_page_size);
        if (huge_start <= addr && addr < huge_end) {
            addr = align_down(addr, huge_page_size);
            size = huge_page_size;
        }
    }
    populate_anon(reinterpret_cast<void*>(r->span.start),
                  reinterpret_cast<void*>(addr), size, r->perm,
                  is_page_fault_write(error), t->small_pages, t->zero);
    trace_mmu_vm_fault_ret(addr, error);
}

error mincore(const void *addr, size_t length, unsigned char *vec)
{
    char *end = align_up((char *)addr + length, page_size);
    char tmp;
    if (!is_linear_mapped(addr, length) && !ismapped(addr, length)) {
        return make_error(ENOMEM);
    }
    for (char *p = (char *)addr; p < end; p += page_size) {
        if (safe_load(p, tmp)) {
            *vec++ = 0x01;
        } else {
            *vec++ = 0x00;
        }
    }
    return no_error();
}

}

extern "C" bool is_linear_mapped(const void *addr)
{
    return addr >= mmu::phys_mem;
}
