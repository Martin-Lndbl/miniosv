#ifndef BYPASS_BIT_H
#define BYPASS_BIT_H


#include <cstdint>
#define RTE_BIT32(x) (1u << x)
#define RTE_BIT64(x) (1ull << x)
#define RTE_DIM(a)  (sizeof (a) / sizeof ((a)[0]))
#define RTE_MIN(x, y)(x < y ? x : y)
#define RTE_MAX(x, y)(x > y ? x : y)

__inline constexpr uint32_t rte_align32prevpow2(uint32_t num){
    return num ? (1u << (sizeof(num) * 8 - __builtin_clz(num) - 1)) : 0;
}

__inline constexpr bool rte_is_power_of_2(uint32_t val){
    return __builtin_popcount(val) == 1;
}
#endif

