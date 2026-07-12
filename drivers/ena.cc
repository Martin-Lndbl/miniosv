/*
 * Copyright (C) 2023 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <sys/cdefs.h>

#include "drivers/ena.hh"
#include "drivers/pci-device.hh"

#include <osv/aligned_new.hh>

namespace aws {

#define ena_tag "ena"
#define ena_d(...)   tprintf_d(ena_tag, __VA_ARGS__)
#define ena_i(...)   tprintf_i(ena_tag, __VA_ARGS__)
#define ena_w(...)   tprintf_w(ena_tag, __VA_ARGS__)
#define ena_e(...)   tprintf_e(ena_tag, __VA_ARGS__)

ena::ena(pci::device &dev)
    : _dev(dev)
{
    _adapter = nullptr;
    auto ret = ena_attach(&_dev, &_adapter);
    if (ret || !_adapter) {
       throw std::runtime_error("Failed to attach ena device");
    }
}

ena::~ena()
{
    ena_detach(_adapter);
    _adapter = nullptr;
}

void ena::dump_config(void)
{
    u8 B, D, F;
    _dev.get_bdf(B, D, F);

    _dev.dump_config();
    ena_d("%s [%x:%x.%x] vid:id= %x:%x", get_name().c_str(),
        (u16)B, (u16)D, (u16)F,
        _dev.get_vendor_id(),
        _dev.get_device_id());
}

hw_driver* ena::probe(hw_device* dev)
{
    try {
        if (auto pci_dev = dynamic_cast<pci::device*>(dev)) {
            if (ena_probe(pci_dev)) {
                return aligned_new<ena>(*pci_dev);
            }
        }
    } catch (std::exception& e) {
        ena_e("Exception on device construction: %s", e.what());
    }
    return nullptr;
}

}
