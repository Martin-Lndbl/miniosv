/*
 * Shared between sched.cc and worker.cc; not part of the engine interface.
 */

#ifndef LROS_INTERNAL_HH
#define LROS_INTERNAL_HH

#include <osv/mutex.h>

#include "include/lros.hh"

namespace lros {

extern mutex lock;
extern engine_ops ops;

uint64_t now_ns();

// sched.cc, lock held by the caller.
void schedule();
assignment take_assignment(task *t);

// sched.cc, take the lock themselves.
void task_phase_changed(task *t);
void task_done(task *t);
void core_freed();

// worker.cc: creates the main worker of a task on one of its cores. Lock held.
void worker_start(task *t);

}

#endif /* LROS_INTERNAL_HH */
