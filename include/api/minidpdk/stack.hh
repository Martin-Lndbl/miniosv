#pragma once

#include <cstring>
#include <minidpdk/util.hh>
#include <osv/mutex.h>
#include <osv/preempt-lock.hh>
#include <osv/sched.hh>
#include <vector>

struct stack {
  std::vector<void *> objs;
  size_t head = 0;

  stack(size_t size) : objs(size) {}

  static stack *create(size_t size) { return new stack(size); }

  static void destroy(stack *s) { delete s; }

  unsigned int push(void *const *obj_table, unsigned int n) {
    WITH_LOCK(preempt_lock) {
      if (unlikely(objs.size() - head < n))
        return 0;
      std::memcpy(&objs[head], obj_table, n * sizeof(void *));
      head += n;
    }
    return n;
  }

  unsigned int pop(void **obj_table, unsigned int n) {
    WITH_LOCK(preempt_lock) {
      if (unlikely(head < n))
        return 0;
      for (unsigned i = 0; i < n; ++i)
        obj_table[n - i - 1] = objs[head - n + i];
      head -= n;
    }
    return n;
  }
};
