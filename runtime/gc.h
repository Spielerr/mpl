/* Copyright (C) 2019-2020 Matthew Fluet.
 * Copyright (C) 1999-2008 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 */

#ifndef _MLTON_GC_H_
#define _MLTON_GC_H_

#include "platform.h"

struct GC_state;
typedef struct GC_state *GC_state;
typedef GC_state GCState_t;

#if POINTER_BITS == 32
#define GC_MODEL_NATIVE32
#elif POINTER_BITS == 64
#define GC_MODEL_NATIVE64
#else
#error POINTER_BITS not defined
#endif

#include "gc/gdtoa-multiple-threads-defs.h"

#include "gc/debug.h"
#include "gc/logger.h"
#include "gc/sampler.h"
#include "gc/block-allocator.h"
#include "gc/time-histogram.h"

#include "gc/tls-objects.h"
#include "gc/align.h"
#include "gc/model.h"
#include "gc/pointer.h"
#include "gc/objptr.h"
#include "gc/object.h"
#include "gc/decheck.h"
#include "gc/sequence.h"
#include "gc/sources.h"
#include "gc/frame.h"
#include "gc/static-heaps.h"
#include "gc/foreach.h"
#include "gc/stack.h"
#include "gc/chunk.h"
#include "gc/fixed-size-allocator.h"
#include "gc/thread.h"
#include "gc/weak.h"
#include "gc/int-inf.h"
#include "gc/string.h"
#include "gc/object-size.h"
#include "gc/heap.h"
#include "gc/current.h"
#include "gc/sysvals.h"
#include "gc/controls.h"
#include "gc/major.h"
#include "gc/statistics.h"
#include "gc/forward.h"
#include "gc/invariant.h"
#include "gc/atomic.h"
#include "gc/enter_leave.h"
#include "gc/signals.h"
#include "gc/handler.h"
#include "gc/switch-thread.h"
#include "gc/garbage-collection.h"
#include "gc/new-object.h"
#include "gc/sequence-allocate.h"
#include "gc/call-stack.h"
#include "gc/profiling.h"
#include "gc/rusage.h"
#include "gc/termination.h"
#include "gc/gc_state.h"
#include "gc/init-world.h"
#include "gc/world.h"
#include "gc/init.h"
#include "gc/done.h"
#include "gc/copy-thread.h"
#include "gc/pack.h"
#include "gc/size.h"
#include "gc/share.h"
#include "gc/parallel.h"
#include "gc/processor.h"
#include "gc/pin.h"
#include "gc/hierarchical-heap.h"
#include "gc/ebr.h"
#include "gc/hierarchical-heap-ebr.h"
#include "gc/entangled-ebr.h"
#include "gc/hierarchical-heap-collection.h"
#include "gc/entanglement-suspects.h"
#include "gc/local-scope.h"
#include "gc/local-heap.h"
#include "gc/abp-deque.h"
#include "gc/assign.h"
#include "gc/concurrent-list.h"
#include "gc/remembered-set.h"
#include "gc/gap.h"
// #include "gc/deferred-promote.h"
#include "gc/tracing-hooks.h"

#endif /* _MLTON_GC_H_ */
