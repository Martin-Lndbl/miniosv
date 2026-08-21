/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_PCI_HH
#define ARCH_PCI_HH

#include "processor.hh"
#include "drivers/pci-device.hh"
#include <osv/interrupt.hh>

namespace pci {

using processor::inb;
using processor::inw;
using processor::inl;
using processor::outb;
using processor::outw;
using processor::outl;

// Bounds of arch_add_bar's MMIO pool; the bridge driver needs them to
// widen its memory windows to cover any BAR we hand out from here.
u64 get_pci_mem_base();
u64 get_pci_mem_end();

} /* namespace pci */

class pci_interrupt : public gsi_level_interrupt {
public:
    pci_interrupt(pci::device &dev, std::function<bool ()> a, std::function<void ()> h)
        : gsi_level_interrupt(dev.get_interrupt_line(), a, h) {};
};

#endif /* ARCH_PCI_HH */
