/*
 * Copyright (C) 2023 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ENA_HH
#define ENA_HH

#include "drivers/driver.hh"
#include "drivers/pci-device.hh"

struct ena_adapter;

bool ena_probe(pci::device *dev);
int ena_attach(pci::device *dev,  ena_adapter **_adapter);
int ena_detach(ena_adapter *adapter);

namespace aws {

class ena: public hw_driver {
public:
    explicit ena(pci::device& dev);
    virtual ~ena();

    virtual std::string get_name() const { return "ena"; }

    virtual void dump_config(void);

    static hw_driver* probe(hw_device* dev);

private:
    pci::device& _dev;
    ena_adapter *_adapter;
};

}

#endif
