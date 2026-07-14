#ifndef BYPASS_MEM_H
#define BYPASS_MEM_H

#include "minidpdk/slab.hh"
#include <minidpdk/time.hh>
#include <minidpdk/util.hh>
#include <cassert>
#include <cstdint>
#include <osv/types.h>

#define RTE_MBUF_F_RX_VLAN (1ULL << 0)

#define RTE_MBUF_F_RX_RSS_HASH (1ULL << 1)

#define RTE_MBUF_F_RX_FDIR (1ULL << 2)

#define RTE_MBUF_F_RX_OUTER_IP_CKSUM_BAD (1ULL << 5)

#define RTE_MBUF_F_RX_VLAN_STRIPPED (1ULL << 6)

#define RTE_MBUF_F_RX_IP_CKSUM_MASK ((1ULL << 4) | (1ULL << 7))

#define RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_IP_CKSUM_BAD (1ULL << 4)
#define RTE_MBUF_F_RX_IP_CKSUM_GOOD (1ULL << 7)
#define RTE_MBUF_F_RX_IP_CKSUM_NONE ((1ULL << 4) | (1ULL << 7))

#define RTE_MBUF_F_RX_L4_CKSUM_MASK ((1ULL << 3) | (1ULL << 8))

#define RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_L4_CKSUM_BAD (1ULL << 3)
#define RTE_MBUF_F_RX_L4_CKSUM_GOOD (1ULL << 8)
#define RTE_MBUF_F_RX_L4_CKSUM_NONE ((1ULL << 3) | (1ULL << 8))

#define RTE_MBUF_F_RX_IEEE1588_PTP (1ULL << 9)

#define RTE_MBUF_F_RX_IEEE1588_TMST (1ULL << 10)

#define RTE_MBUF_F_RX_FDIR_ID (1ULL << 13)

#define RTE_MBUF_F_RX_FDIR_FLX (1ULL << 14)

#define RTE_MBUF_F_RX_QINQ_STRIPPED (1ULL << 15)

#define RTE_MBUF_F_RX_LRO (1ULL << 16)

/* There is no flag defined at offset 17. It is free for any future use. */

#define RTE_MBUF_F_RX_SEC_OFFLOAD (1ULL << 18)

#define RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED (1ULL << 19)

#define RTE_MBUF_F_RX_QINQ (1ULL << 20)

#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_MASK ((1ULL << 21) | (1ULL << 22))

#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_UNKNOWN 0
#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_BAD (1ULL << 21)
#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_GOOD (1ULL << 22)
#define RTE_MBUF_F_RX_OUTER_L4_CKSUM_INVALID ((1ULL << 21) | (1ULL << 22))

/* add new RX flags here, don't forget to update RTE_MBUF_F_FIRST_FREE */

#define RTE_MBUF_F_FIRST_FREE (1ULL << 23)
#define RTE_MBUF_F_LAST_FREE (1ULL << 40) 

/* add new TX flags here, don't forget to update RTE_MBUF_F_LAST_FREE  */

#define RTE_MBUF_F_TX_OUTER_UDP_CKSUM (1ULL << 41)

#define RTE_MBUF_F_TX_UDP_SEG (1ULL << 42)

#define RTE_MBUF_F_TX_SEC_OFFLOAD (1ULL << 43)

#define RTE_MBUF_F_TX_MACSEC (1ULL << 44)

#define RTE_MBUF_F_TX_TUNNEL_VXLAN (0x1ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_GRE (0x2ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_IPIP (0x3ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_GENEVE (0x4ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_MPLSINUDP (0x5ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_VXLAN_GPE (0x6ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_GTP (0x7ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_ESP (0x8ULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_IP (0xDULL << 45)
#define RTE_MBUF_F_TX_TUNNEL_UDP (0xEULL << 45)
/* add new TX TUNNEL type here */
#define RTE_MBUF_F_TX_TUNNEL_MASK (0xFULL << 45)

#define RTE_MBUF_F_TX_QINQ (1ULL << 49)

#define RTE_MBUF_F_TX_TCP_SEG (1ULL << 50)

#define RTE_MBUF_F_TX_IEEE1588_TMST (1ULL << 51)

/*
 * Bits 52+53 used for L4 packet type with checksum enabled: 00: Reserved,
 * 01: TCP checksum, 10: SCTP checksum, 11: UDP checksum. To use hardware
 * L4 checksum offload, the user needs to:
 *  - fill l2_len and l3_len in mbuf
 *  - set the flags RTE_MBUF_F_TX_TCP_CKSUM, RTE_MBUF_F_TX_SCTP_CKSUM or
 *    RTE_MBUF_F_TX_UDP_CKSUM
 *  - set the flag RTE_MBUF_F_TX_IPV4 or RTE_MBUF_F_TX_IPV6
 */

#define RTE_MBUF_F_TX_L4_NO_CKSUM (0ULL << 52)

#define RTE_MBUF_F_TX_TCP_CKSUM (1ULL << 52)

#define RTE_MBUF_F_TX_SCTP_CKSUM (2ULL << 52)

#define RTE_MBUF_F_TX_UDP_CKSUM (3ULL << 52)

#define RTE_MBUF_F_TX_L4_MASK (3ULL << 52)

#define RTE_MBUF_F_TX_IP_CKSUM (1ULL << 54)

#define RTE_MBUF_F_TX_IPV4 (1ULL << 55)

#define RTE_MBUF_F_TX_IPV6 (1ULL << 56)

#define RTE_MBUF_F_TX_VLAN (1ULL << 57)

#define RTE_MBUF_F_TX_OUTER_IP_CKSUM (1ULL << 58)

#define RTE_MBUF_F_TX_OUTER_IPV4 (1ULL << 59)

#define RTE_MBUF_F_TX_OUTER_IPV6 (1ULL << 60)

#define RTE_MBUF_F_TX_OFFLOAD_MASK                                             \
  (RTE_MBUF_F_TX_OUTER_IPV6 | RTE_MBUF_F_TX_OUTER_IPV4 |                       \
   RTE_MBUF_F_TX_OUTER_IP_CKSUM | RTE_MBUF_F_TX_VLAN | RTE_MBUF_F_TX_IPV6 |    \
   RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_L4_MASK |       \
   RTE_MBUF_F_TX_IEEE1588_TMST | RTE_MBUF_F_TX_TCP_SEG | RTE_MBUF_F_TX_QINQ |  \
   RTE_MBUF_F_TX_TUNNEL_MASK | RTE_MBUF_F_TX_MACSEC |                          \
   RTE_MBUF_F_TX_SEC_OFFLOAD | RTE_MBUF_F_TX_UDP_SEG |                         \
   RTE_MBUF_F_TX_OUTER_UDP_CKSUM)

#define RTE_MBUF_F_EXTERNAL (1ULL << 61)

#define RTE_MBUF_F_INDIRECT (1ULL << 62)
#define RTE_MBUF_PRIV_ALIGN 8

#define RTE_MBUF_DEFAULT_DATAROOM 2048
#define RTE_MBUF_DEFAULT_BUF_SIZE (RTE_MBUF_DEFAULT_DATAROOM)

#define SOCKET_ID_ANY (-1)

template <typename T, T alignment> static constexpr T align(T val) {
  return (val + alignment - 1) & ~(alignment - 1);
}

#define rte_free free

using rte_mbuf = minidpdk::mbuf;
using rte_pktmbuf_pool = minidpdk::mem_pool;
using rte_mempool = rte_pktmbuf_pool;
using rte_mbuf_ext_shared_info = minidpdk::rte_mbuf_ext_shared_info;

inline void rte_pktmbuf_free_helper(rte_mbuf *buf) {
  if (buf->ol_flags & RTE_MBUF_F_EXTERNAL || buf->shinfo) {
    assert(buf->shinfo->refcnt > 0);
    if (--buf->shinfo->refcnt == 0)
      buf->shinfo->free_cb(buf->buf_addr, buf->shinfo->fcb_opaque);
  }
  minidpdk::mbuf_free(buf);
}
inline void rte_pktmbuf_free(rte_mbuf *m) { rte_pktmbuf_free_helper(m); }
inline void rte_mbuf_raw_free(rte_mbuf *m) { rte_pktmbuf_free_helper(m); }
inline int rte_pktmbuf_alloc_bulk(rte_mempool *pool, rte_mbuf **pkts, uint16_t size) {
  int ret = pool->alloc_bulk(reinterpret_cast<void **>(pkts), size);
  if (ret < 0) return ret;
  if (pool->init_fn) pool->init_fn(pkts, size, pool->priv);
  return 0;
}
inline void rte_pktmbuf_free_bulk(rte_mbuf **pkts, uint16_t size) {
  for (auto i = 0u; i < size; ++i) rte_pktmbuf_free_helper(pkts[i]);
}
__inline rte_mbuf* rte_pktmbuf_alloc(rte_mempool* pool){
    return pool->alloc_single();
}
inline const void *rte_pktmbuf_read(rte_mbuf *m, uint32_t off, uint32_t len,
                                    uint8_t *buf) {
  return m->read(off, len, buf);
}
inline rte_mempool *rte_pktmbuf_pool_create(const char *name, unsigned n,
                                            unsigned cache_size,
                                            uint16_t priv_size,
                                            uint16_t data_room_size,
                                            int socket_id) {
  (void)name; (void)cache_size; (void)priv_size;
  (void)data_room_size; (void)socket_id;
  assert(data_room_size <= minidpdk::mem_pool::kMaxDataLen);
  return new minidpdk::mem_pool(n);
}
inline void rte_mempool_free(rte_mempool *pool) { delete pool; }

inline void rte_pktmbuf_attach_extbuf(rte_mbuf* m, void* buf_addr, uintptr_t iova, uint16_t buf_len, rte_mbuf_ext_shared_info* shinfo){
    m->shinfo = shinfo;
    m->buf_addr = static_cast<char*>(buf_addr);
    m->iova = iova;
    m->buf_len = buf_len;
    m->data_len = 0;
    m->data_offset = 0;
    m->ol_flags |= RTE_MBUF_F_EXTERNAL;
}

inline int rte_pktmbuf_chain(rte_mbuf* head, rte_mbuf *tail){
    auto *cur_tail = head->last_seg();
    cur_tail->next = tail;
    head->nb_segs += tail->nb_segs;
    head->pkt_len += tail->pkt_len;
    tail->pkt_len = tail->data_len;
    return 0;
}

inline void rte_pktmbuf_detach(rte_mbuf* mbuf){
    mbuf->shinfo = nullptr;
    mbuf->ol_flags &= ~RTE_MBUF_F_EXTERNAL;
}

#define rte_pktmbuf_mtod(m, t) m->data<std::remove_pointer<t>::type>()
#define rte_pktmbuf_mtod_offset(m, t, o)                                       \
  m->data<std::remove_pointer<t>::type>(o)
#endif // !BYPASS_MEM_H
