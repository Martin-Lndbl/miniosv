/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef EXECINFO_HH_
#define EXECINFO_HH_

namespace osv {

// Stack unwinder behind backtrace(), also used directly by the panic/abort
// path. Walks .eh_frame via libgcc's _Unwind_Backtrace, so no frame pointers
// are required. Fills `pc` with up to `nr` return addresses, starting at the
// direct caller, and returns how many it wrote. If `cfa` is non-null it also
// receives the canonical frame address of each frame, which is useful for
// spotting recursion / stack overflow.
int unwind(void** pc, unsigned long* cfa, int nr);

// Called by the arch page-fault handlers before they commit to handling a
// fault, with the pc they would resume at. An unwind that walked off a bad
// canonical frame address must not take the kernel down with it, so if this
// fault came from one, `pc` is replaced with a landing pad that abandons the
// walk and true is returned; the handler should then just return.
bool unwind_recover_from_fault(void** pc);

// How many unwinds have been abandoned that way, i.e. how many backtraces came
// back truncated because the walk read from a frame address that was not
// there. Expected to be small but not zero: an interrupt can land anywhere,
// including in code whose CFI does not describe a whole frame yet.
unsigned long unwind_faults();

// Declare any unwind this thread had in progress dead, so the next one is not
// turned away as a re-entry. For the panic path: a panic raised while a
// backtrace was being taken would otherwise print no backtrace at all, which
// is the one moment it is most wanted.
void unwind_abandon();

}

#endif /* EXECINFO_HH_ */
