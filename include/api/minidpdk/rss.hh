#ifndef RSS_H_
#define RSS_H_

#include <cstdint>

#ifndef RTE_BIT64
#define RTE_BIT64(x) (1ull << x)
#endif // !BIT

#ifndef RTE_BIT32
#define RTE_BIT32(x) (1u << x)
#endif // !RTE_BIT32

/* packet fields */
#define RTE_ETH_RSS_IPV4               RTE_BIT64(2)
#define RTE_ETH_RSS_FRAG_IPV4          RTE_BIT64(3)
#define RTE_ETH_RSS_NONFRAG_IPV4_TCP   RTE_BIT64(4)
#define RTE_ETH_RSS_NONFRAG_IPV4_UDP   RTE_BIT64(5)
#define RTE_ETH_RSS_NONFRAG_IPV4_SCTP  RTE_BIT64(6)
#define RTE_ETH_RSS_NONFRAG_IPV4_OTHER RTE_BIT64(7)
#define RTE_ETH_RSS_IPV6               RTE_BIT64(8)
#define RTE_ETH_RSS_FRAG_IPV6          RTE_BIT64(9)
#define RTE_ETH_RSS_NONFRAG_IPV6_TCP   RTE_BIT64(10)
#define RTE_ETH_RSS_NONFRAG_IPV6_UDP   RTE_BIT64(11)
#define RTE_ETH_RSS_NONFRAG_IPV6_SCTP  RTE_BIT64(12)
#define RTE_ETH_RSS_NONFRAG_IPV6_OTHER RTE_BIT64(13)
#define RTE_ETH_RSS_L2_PAYLOAD         RTE_BIT64(14)
#define RTE_ETH_RSS_IPV6_EX            RTE_BIT64(15)
#define RTE_ETH_RSS_IPV6_TCP_EX        RTE_BIT64(16)
#define RTE_ETH_RSS_IPV6_UDP_EX        RTE_BIT64(17)
#define RTE_ETH_RSS_PORT               RTE_BIT64(18)
#define RTE_ETH_RSS_VXLAN              RTE_BIT64(19)
#define RTE_ETH_RSS_GENEVE             RTE_BIT64(20)
#define RTE_ETH_RSS_NVGRE              RTE_BIT64(21)
#define RTE_ETH_RSS_GTPU               RTE_BIT64(23)
#define RTE_ETH_RSS_ETH                RTE_BIT64(24)
#define RTE_ETH_RSS_S_VLAN             RTE_BIT64(25)
#define RTE_ETH_RSS_C_VLAN             RTE_BIT64(26)
#define RTE_ETH_RSS_ESP                RTE_BIT64(27)
#define RTE_ETH_RSS_AH                 RTE_BIT64(28)
#define RTE_ETH_RSS_L2TPV3             RTE_BIT64(29)
#define RTE_ETH_RSS_PFCP               RTE_BIT64(30)
#define RTE_ETH_RSS_PPPOE              RTE_BIT64(31)
#define RTE_ETH_RSS_ECPRI              RTE_BIT64(32)
#define RTE_ETH_RSS_MPLS               RTE_BIT64(33)
#define RTE_ETH_RSS_IPV4_CHKSUM        RTE_BIT64(34)
 
#define RTE_ETH_RSS_L4_CHKSUM          RTE_BIT64(35)
 
#define RTE_ETH_RSS_L2TPV2             RTE_BIT64(36)
#define RTE_ETH_RSS_IPV6_FLOW_LABEL    RTE_BIT64(37)
 
#define RTE_ETH_RSS_IB_BTH             RTE_BIT64(38)

#define RTE_ETH_RSS_L3_SRC_ONLY        RTE_BIT64(63)
#define RTE_ETH_RSS_L3_DST_ONLY        RTE_BIT64(62)
#define RTE_ETH_RSS_L4_SRC_ONLY        RTE_BIT64(61)
#define RTE_ETH_RSS_L4_DST_ONLY        RTE_BIT64(60)
#define RTE_ETH_RSS_L2_SRC_ONLY        RTE_BIT64(59)
#define RTE_ETH_RSS_L2_DST_ONLY        RTE_BIT64(58)


enum rte_eth_hash_function {
    RTE_ETH_HASH_FUNCTION_DEFAULT = 0,
    RTE_ETH_HASH_FUNCTION_TOEPLITZ, 
    RTE_ETH_HASH_FUNCTION_SIMPLE_XOR, 
    RTE_ETH_HASH_FUNCTION_SYMMETRIC_TOEPLITZ,
    RTE_ETH_HASH_FUNCTION_SYMMETRIC_TOEPLITZ_SORT,
    RTE_ETH_HASH_FUNCTION_MAX,
};
 
#define RTE_ETH_HASH_ALGO_TO_CAPA(x) RTE_BIT32(x)
#define RTE_ETH_HASH_ALGO_CAPA_MASK(x) RTE_BIT32(RTE_ETH_HASH_FUNCTION_ ## x)
 
struct rte_eth_rss_conf {
    uint8_t *rss_key;
    uint8_t rss_key_len; 
    uint64_t rss_hf;
    enum rte_eth_hash_function algorithm;   
};

#define RTE_ETH_RSS_RETA_SIZE_64  64
#define RTE_ETH_RSS_RETA_SIZE_128 128
#define RTE_ETH_RSS_RETA_SIZE_256 256
#define RTE_ETH_RSS_RETA_SIZE_512 512
#define RTE_ETH_RETA_GROUP_SIZE   64

#define RTE_ETH_VMDQ_NUM_UC_HASH_ARRAY 128 
#define RTE_ETH_VMDQ_ACCEPT_UNTAG      RTE_BIT32(0)
#define RTE_ETH_VMDQ_ACCEPT_HASH_MC    RTE_BIT32(1)
#define RTE_ETH_VMDQ_ACCEPT_HASH_UC    RTE_BIT32(2)
#define RTE_ETH_VMDQ_ACCEPT_BROADCAST  RTE_BIT32(3)
#define RTE_ETH_VMDQ_ACCEPT_MULTICAST  RTE_BIT32(4)
struct rte_eth_rss_reta_entry64 {
    uint64_t mask;
    uint16_t reta[RTE_ETH_RETA_GROUP_SIZE];
};

#endif // !RSS_H_
