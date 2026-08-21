#ifndef BYPASS_NET_H
#define BYPASS_NET_H

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <endian.h>

static constexpr uint8_t IPVERSION = 4;
static constexpr uint8_t TTL = 64;
static constexpr uint16_t RTE_ETHER_TYPE_IPV4 = 0x0800;
static constexpr uint8_t RTE_IPPROTO_UDP = 17;

#define RTE_ETHER_ADDR_LEN 6
#define RTE_IPV4_HDR_DF_SHIFT   14 
#define RTE_IPV4_HDR_DF_FLAG    (1 << RTE_IPV4_HDR_DF_SHIFT)
#define RTE_IPV4_MIN_IHL    (0x5)
#define RTE_IPV4_VHL_DEF    ((IPVERSION << 4) | RTE_IPV4_MIN_IHL)

struct rte_ether_addr {
  std::array<unsigned char, RTE_ETHER_ADDR_LEN> addr;
  void parse_string(const char *mac) {
    sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &addr[0], &addr[1], &addr[2],
           &addr[3], &addr[4], &addr[5]);
  }

  static rte_ether_addr broadcast;
};

inline void rte_ether_addr_copy(const rte_ether_addr* src, rte_ether_addr* to){
    *to = *src;
}

struct [[gnu::packed]] rte_ether_hdr {
  rte_ether_addr dst_addr;
  rte_ether_addr src_addr;
  uint16_t ether_type;
};

struct [[gnu::packed]] rte_ipv4_hdr {
  uint8_t version_ihl;
  uint8_t type_of_service;
  uint16_t total_length;
  uint16_t packet_id;
  uint16_t fragment_offset;
  uint8_t time_to_live;
  uint8_t next_proto_id;
  uint16_t hdr_checksum;
  uint32_t src_addr;
  uint32_t dst_addr;
};

struct [[gnu::packed]] rte_udp_hdr {
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t dgram_len;
  uint16_t dgram_cksum;
};

__inline void *PTR_ADD(const void *ptr, size_t x) {
  return ((void *)((uintptr_t)(ptr) + (x)));
}

template <typename T> __inline T ALIGN_FLOOR(T val, uint32_t align) {
  return static_cast<T>((val) & (~static_cast<T>(align - 1)));
}

/* from dpdk */

inline uint32_t _raw_cksum(const void *buf, size_t len, uint32_t sum) {
  const void *end;

  for (end = PTR_ADD(buf, ALIGN_FLOOR(len, sizeof(uint16_t))); buf != end;
       buf = PTR_ADD(buf, sizeof(uint16_t))) {
    uint16_t v;

    memcpy(&v, buf, sizeof(uint16_t));
    sum += v;
  }

  /* if length is odd, keeping it byte order independent */
  if (len % 2) {
    uint16_t left = 0;

    memcpy(&left, end, 1);
    sum += left;
  }

  return sum;
}

inline uint16_t _raw_cksum_reduce(uint32_t sum) {
  sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
  sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
  return (uint16_t)sum;
}


inline uint16_t rte_ipv4_phdr_cksum(rte_ipv4_hdr *ipv4, uint64_t ol_flags) {
  struct ipv4_psd_header {
    uint32_t src_addr; /* IP address of source host. */
    uint32_t dst_addr; /* IP address of destination host. */
    uint8_t zero;      /* zero. */
    uint8_t proto;     /* L4 protocol type. */
    uint16_t len;      /* L4 length. */
  } psd_hdr;

  psd_hdr.src_addr = ipv4->src_addr;
  psd_hdr.dst_addr = ipv4->dst_addr;
  psd_hdr.zero = 0;
  psd_hdr.proto = ipv4->next_proto_id;
  psd_hdr.len = htobe16(betoh16(ipv4->total_length) - sizeof(*ipv4));
  auto sum = _raw_cksum(&psd_hdr, sizeof(psd_hdr), 0);
  return _raw_cksum_reduce(sum);
}
#endif // !BYPASS_NET_H
