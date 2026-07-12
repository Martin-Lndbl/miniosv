#include <minidpdk/dev.hh>
#include <minidpdk/lcore.hh>
#include <cstdint>

eth_os eth_os::instance;
lcore_container lcore_container::instance;

void eth_os::register_port(rte_eth_dev *dev){ 
    instance.ifs.push_back(dev);
}

int rte_eth_dev::dev_configure(uint16_t nb_rx, uint16_t nb_tx, rte_eth_conf *conf){
    data.nb_rx_queues = nb_rx;
    data.nb_tx_queues = nb_tx;
    data.dev_conf.rxmode = conf->rxmode;
    data.dev_conf.txmode = conf->txmode;
    data.dev_conf.rx_adv_conf = conf->rx_adv_conf;
    data.tx_queues.resize(nb_tx, nullptr);
    data.rx_queues.resize(nb_rx, nullptr);
    data.tx_queue_state.resize(nb_tx, RTE_ETH_QUEUE_STATE_STOPPED);
    data.rx_queue_state.resize(nb_rx, RTE_ETH_QUEUE_STATE_STOPPED);
    return drv_configure();
}
