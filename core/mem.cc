#include <minidpdk/mem.hh>
#include <minidpdk/slab.hh>
#include <cassert>
#include <cstdint>
#include <malloc.h>
#include <osv/trace.hh>

void inline free_internal(rte_mbuf* buf){
  if(buf->ol_flags & RTE_MBUF_F_EXTERNAL || buf->shinfo){
      assert(buf->shinfo->refcnt > 0);
      --buf->shinfo->refcnt;
      if(!buf->shinfo->refcnt)
          buf->shinfo->free_cb(buf->buf_addr, buf->shinfo->fcb_opaque);
  }
  minidpdk::mbuf_free(buf);
}

void rte_pktmbuf_free(rte_mbuf* mbuf){
    free_internal(mbuf);
}
void rte_mbuf_raw_free(rte_mbuf* mbuf){
    free_internal(mbuf);
}


int rte_pktmbuf_alloc_bulk(rte_mempool* pool, rte_mbuf** pkts, uint16_t size){
    int ret = pool->alloc_bulk(reinterpret_cast<void**>(pkts), size);
    if(ret < 0)
        return ret;
    if(pool->init_fn)
        pool->init_fn(pkts, size, pool->priv);
    return 0;
}

void rte_pktmbuf_free_bulk(rte_mbuf** pkts, uint16_t size){
    for(auto i = 0u; i < size; ++i)
        free_internal(pkts[i]);
}

const void* rte_pktmbuf_read(rte_mbuf *m, uint32_t off,
	uint32_t len, uint8_t *buf)
{
    return m->read(off, len, buf);
}

rte_mempool *rte_pktmbuf_pool_create(const char *name, unsigned n,
                                     unsigned cache_size, uint16_t priv_size,
                                     uint16_t data_room_size, int socket_id){
    assert(data_room_size <= minidpdk::mem_pool::kMaxDataLen);
    (void)cache_size;
    (void)priv_size;
    (void)socket_id;
    (void)data_room_size;
    auto *slab = malloc(sizeof(minidpdk::mem_pool));
    return new(slab) minidpdk::mem_pool(n);
}
void rte_mempool_free(rte_mempool *pool){
    pool->~mem_pool();
    free(pool);
}

