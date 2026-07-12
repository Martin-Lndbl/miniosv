#ifndef ARCH_BARRIER_HH
#define ARCH_BARRIER_HH

#define mb()    asm volatile("mfence" ::: "memory")
#define wmb()   asm volatile("sfence" ::: "memory")
#define rmb()   asm volatile("lfence" ::: "memory")

#endif
