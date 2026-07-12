#ifndef ARCH_BARRIER_HH
#define ARCH_BARRIER_HH

#define mb()    asm volatile("dsb sy" ::: "memory")
#define wmb()   asm volatile("dsb st" ::: "memory")
#define rmb()   asm volatile("dsb ld" ::: "memory")

#endif
