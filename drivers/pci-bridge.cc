/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>

#include <osv/debug.hh>
#include <osv/pci.hh>
#include "drivers/pci-function.hh"
#include "drivers/pci-bridge.hh"

namespace pci {

    // PCI-to-PCI bridge (header type 1) memory-window registers.
    // Base/Limit low words hold bits [31:20] in bits [15:4]; low 20 bits
    // are implicit 0/1, giving 1 MiB granularity. Prefetchable base's
    // bits[3:0]=0x1 signals the bridge supports 64-bit windows.
    static constexpr u8 BRIDGE_MEM_BASE       = 0x20;
    static constexpr u8 BRIDGE_MEM_LIMIT      = 0x22;
    static constexpr u8 BRIDGE_PREF_BASE      = 0x24;
    static constexpr u8 BRIDGE_PREF_LIMIT     = 0x26;
    static constexpr u8 BRIDGE_PREF_BASE_HI   = 0x28;
    static constexpr u8 BRIDGE_PREF_LIMIT_HI  = 0x2C;

    bridge::bridge(u8 bus, u8 device, u8 func)
        : function(bus, device, func)
    {

    }

    bridge::~bridge()
    {

    }

    bool bridge::parse_pci_config()
    {
        function::parse_pci_config();

        // Some AWS Nitro Gen 5 configs (c7i.large) leave downstream
        // bridges with empty memory windows (limit < base). BARs behind
        // such a bridge are unreachable until we program the window.
        // On platforms where firmware set a valid window we take the
        // union with our pool so the pool is always covered without
        // dropping firmware's assignment.

        u64 pool_base = get_pci_mem_base();
        u64 pool_end  = get_pci_mem_end() - 1;               // inclusive
        u64 pool_base_1m = pool_base & ~0xFFFFFull;          // 1 MiB align down
        u64 pool_end_1m  = pool_end  |  0xFFFFFull;          // 1 MiB align up

        u16 mem_base_raw  = pci_readw(BRIDGE_MEM_BASE);
        u16 mem_limit_raw = pci_readw(BRIDGE_MEM_LIMIT);
        u64 mem_base  = ((u64)(mem_base_raw  & 0xFFF0)) << 16;
        u64 mem_limit = (((u64)(mem_limit_raw & 0xFFF0)) << 16) | 0xFFFFFull;
        bool mem_empty = mem_limit < mem_base;

        u16 pref_base_raw  = pci_readw(BRIDGE_PREF_BASE);
        u16 pref_limit_raw = pci_readw(BRIDGE_PREF_LIMIT);
        bool pref_64 = (pref_base_raw & 0xF) == 0x1;
        u64 pref_base  = ((u64)(pref_base_raw  & 0xFFF0)) << 16;
        u64 pref_limit = (((u64)(pref_limit_raw & 0xFFF0)) << 16) | 0xFFFFFull;
        if (pref_64) {
            pref_base  |= ((u64)pci_readl(BRIDGE_PREF_BASE_HI))  << 32;
            pref_limit |= ((u64)pci_readl(BRIDGE_PREF_LIMIT_HI)) << 32;
        }
        bool pref_empty = pref_limit < pref_base;

        u64 new_mem_base  = mem_empty ? pool_base_1m : std::min(mem_base,  pool_base_1m);
        u64 new_mem_limit = mem_empty ? pool_end_1m  : std::max(mem_limit, pool_end_1m);
        u64 new_pref_base  = pref_empty ? pool_base_1m : std::min(pref_base,  pool_base_1m);
        u64 new_pref_limit = pref_empty ? pool_end_1m  : std::max(pref_limit, pool_end_1m);

        bool mem_changed  = mem_empty  || new_mem_base  != mem_base  || new_mem_limit  != mem_limit;
        bool pref_changed = pref_empty || new_pref_base != pref_base || new_pref_limit != pref_limit;

        if (mem_changed) {
            pci_writew(BRIDGE_MEM_BASE,  (u16)((new_mem_base  >> 16) & 0xFFF0));
            pci_writew(BRIDGE_MEM_LIMIT, (u16)((new_mem_limit >> 16) & 0xFFF0));
        }
        if (pref_changed) {
            u16 pref_type_bits = pref_64 ? 0x1 : 0x0;
            pci_writew(BRIDGE_PREF_BASE,
                       (u16)(((new_pref_base  >> 16) & 0xFFF0) | pref_type_bits));
            pci_writew(BRIDGE_PREF_LIMIT,
                       (u16)(((new_pref_limit >> 16) & 0xFFF0) | pref_type_bits));
            if (pref_64) {
                pci_writel(BRIDGE_PREF_BASE_HI,  (u32)(new_pref_base  >> 32));
                pci_writel(BRIDGE_PREF_LIMIT_HI, (u32)(new_pref_limit >> 32));
            }
        }

        // Bus master must be on for downstream MSI-X writes to reach the
        // LAPIC; mem decode for CPU access to reach downstream BARs.
        u16 command = pci_readw(PCI_CFG_COMMAND);
        u16 new_command = command | PCI_COMMAND_BAR_MEM_ENABLE | PCI_COMMAND_BUS_MASTER;
        if (new_command != command) {
            pci_writew(PCI_CFG_COMMAND, new_command);
        }

        return true;
    }

}
