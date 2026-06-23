// The OSv application.
//
// Unlike upstream OSv, the application is not a separate ELF shared object
// loaded at runtime from a filesystem image. It is compiled and statically
// linked directly into the kernel image (app.o in loader.elf). The kernel
// calls osv_app_main() once, after early initialization, on a dedicated thread.
//
// This is a minimal "hello world" placeholder: it prints a greeting and powers
// the machine off so a run exits cleanly. The conformance and stress suites
// that used to live here now build with `make app=tests` (see test/).

#include <cstdio>
#include <osv/power.hh>
#include <api/minidpdk/dev.hh>

extern "C" void osv_app_main()
{
    auto port_count = eth_os::instance.ifs.size();
 
    if (port_count == 0) {
        printf("nic: no ports registered, no life signal\n");
        osv::poweroff();
        return;
    }
 
    printf("nic: %zu port(s) registered\n", port_count);
 
    for (uint16_t port = 0; port < port_count; ++port) {
        if (!rte_eth_dev_is_valid_port(port)) {
            printf("nic: port %u invalid\n", port);
            continue;
        }
 
        rte_eth_dev_info info{};
        int rc = rte_eth_dev_info_get(port, &info);
        if (rc != 0) {
            printf("nic: port %u did not respond (rc=%d)\n", port, rc);
            continue;
        }
 
        rte_ether_addr mac{};
        rte_eth_macaddr_get(port, &mac);
 
        printf("nic: port %u alive\n", port);
        printf("  MAC address  : %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac.addr[0], mac.addr[1], mac.addr[2],
               mac.addr[3], mac.addr[4], mac.addr[5]);
        printf("  MTU range    : %u..%u\n", info.min_mtu, info.max_mtu);
        printf("  Max queues   : rx=%u tx=%u\n",
               info.max_rx_queues, info.max_tx_queues);
        printf("  Max MAC addrs: %u\n", info.max_mac_addrs);
    }
 
    osv::poweroff();
}
