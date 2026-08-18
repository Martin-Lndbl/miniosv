/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef CONTIGUOUS_ALLOC_HH
#define CONTIGUOUS_ALLOC_HH

#include <cstdlib>

namespace memory {

void* alloc_phys_contiguous_aligned(size_t sz, size_t align, bool block = true);
// The size is the caller's to remember: there is no header in front of the
// allocation to hold it.
void free_phys_contiguous_aligned(void* p, size_t sz);

};

#endif /* CONTIGUOUS_ALLOC_HH */
