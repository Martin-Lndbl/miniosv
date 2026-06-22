#pragma once

#include <cstdint>
#include <osv/sched.hh>
#include <vector>

#define SKIP_MAIN false
#define CALL_MAIN true

struct lcore_container{
    std::vector<sched::thread*> threads;
    uint16_t ncpus;
    static lcore_container instance;
    static void init(uint16_t cnt){
        instance.ncpus = cnt;
        sched::thread::current()->pin(sched::cpus[0]);
        instance.threads.resize(cnt);
        instance.threads.front() = sched::thread::current();
    }
};

inline uint16_t rte_lcore_id(){
    return sched::current_cpu->id;
}

inline uint16_t rte_lcore_count(){
    return lcore_container::instance.ncpus;
}

inline uint16_t rte_lcore_index(uint16_t id){
    (void)id;
    return rte_lcore_id();
}

inline void rte_eal_mp_remote_launch(int(*lcore_fn)(void*), void* arg, bool call_main){
    auto ncpu = lcore_container::instance.ncpus;
    for(auto i = 1u; i < ncpu; ++i){
        lcore_container::instance.threads[i] = sched::thread::make([lcore_fn, arg]{
                    lcore_fn(arg);
                    }, sched::thread::attr().pin(sched::cpus[i]));
        lcore_container::instance.threads[i]->start();
    }
    if(call_main)
        lcore_fn(arg);
}

inline void rte_eal_mp_wait_lcore(){
    for(auto i = 1u; i < lcore_container::instance.ncpus; ++i)
        lcore_container::instance.threads[i]->join();
}

inline uint16_t rte_lcore_to_socket_id(uint16_t){
    return 0;
}

#define RTE_LCORE_FOREACH(id) for(id = 0; id < lcore_container::instance.ncpus; ++id)

