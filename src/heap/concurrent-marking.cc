// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/concurrent-marking.h"

#include <stack>
#include <unordered_map>

#include "src/heap/concurrent-marking-deque.h"
#include "src/heap/heap-inl.h"
#include "src/heap/heap.h"
#include "src/heap/marking.h"
#include "src/heap/objects-visiting-inl.h"
#include "src/heap/objects-visiting.h"
#include "src/isolate.h"
#include "src/locked-queue-inl.h"
#include "src/utils-inl.h"
#include "src/utils.h"
#include "src/v8.h"

namespace v8 {
namespace internal {

// Helper class for storing in-object slot addresses and values.
class SlotSnapshot {
 public:
  SlotSnapshot() : number_of_slots_(0) {}
  int number_of_slots() const { return number_of_slots_; }
  Object** slot(int i) const { return snapshot_[i].first; }
  Object* value(int i) const { return snapshot_[i].second; }
  void clear() { number_of_slots_ = 0; }
  void add(Object** slot, Object* value) {
    snapshot_[number_of_slots_].first = slot;
    snapshot_[number_of_slots_].second = value;
    ++number_of_slots_;
  }

 private:
  static const int kMaxSnapshotSize = JSObject::kMaxInstanceSize / kPointerSize;
  int number_of_slots_;
  std::pair<Object**, Object*> snapshot_[kMaxSnapshotSize];
  DISALLOW_COPY_AND_ASSIGN(SlotSnapshot);
};

class ConcurrentMarkingVisitor final
    : public HeapVisitor<int, ConcurrentMarkingVisitor> {
 public:
  using BaseClass = HeapVisitor<int, ConcurrentMarkingVisitor>;

  explicit ConcurrentMarkingVisitor(ConcurrentMarkingDeque* deque)
      : deque_(deque) {}

  bool ShouldVisit(HeapObject* object) {
    return ObjectMarking::GreyToBlack<AccessMode::ATOMIC>(
        object, marking_state(object));
  }

  void VisitPointers(HeapObject* host, Object** start, Object** end) override {
    for (Object** p = start; p < end; p++) {
      Object* object = reinterpret_cast<Object*>(
          base::Relaxed_Load(reinterpret_cast<const base::AtomicWord*>(p)));
      if (!object->IsHeapObject()) continue;
      MarkObject(HeapObject::cast(object));
    }
  }

  void VisitPointersInSnapshot(const SlotSnapshot& snapshot) {
    for (int i = 0; i < snapshot.number_of_slots(); i++) {
      Object* object = snapshot.value(i);
      if (!object->IsHeapObject()) continue;
      MarkObject(HeapObject::cast(object));
    }
  }

  void VisitCodeEntry(JSFunction* host, Address entry_address) override {
    Address code_entry = base::AsAtomicWord::Relaxed_Load(
        reinterpret_cast<Address*>(entry_address));
    Object* code = Code::GetObjectFromCodeEntry(code_entry);
    VisitPointer(host, &code);
  }

  // ===========================================================================
  // JS object =================================================================
  // ===========================================================================

  int VisitJSObject(Map* map, JSObject* object) {
    int size = JSObject::BodyDescriptor::SizeOf(map, object);
    const SlotSnapshot& snapshot = MakeSlotSnapshot(map, object, size);
    if (!ShouldVisit(object)) return 0;
    VisitPointersInSnapshot(snapshot);
    return size;
  }

  int VisitJSObjectFast(Map* map, JSObject* object) {
    return VisitJSObject(map, object);
  }

  int VisitJSApiObject(Map* map, JSObject* object) {
    return VisitJSObject(map, object);
  }

  // ===========================================================================
  // Fixed array object ========================================================
  // ===========================================================================

  int VisitFixedArray(Map* map, FixedArray* object) {
    int length = object->synchronized_length();
    int size = FixedArray::SizeFor(length);
    if (!ShouldVisit(object)) return 0;
    VisitMapPointer(object, object->map_slot());
    FixedArray::BodyDescriptor::IterateBody(object, size, this);
    return size;
  }

  // ===========================================================================
  // Code object ===============================================================
  // ===========================================================================

  int VisitCode(Map* map, Code* object) {
    deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    return 0;
  }

  // ===========================================================================
  // Objects with weak fields and/or side-effectiful visitation.
  // ===========================================================================

  int VisitBytecodeArray(Map* map, BytecodeArray* object) {
    if (ObjectMarking::IsGrey<AccessMode::ATOMIC>(object,
                                                  marking_state(object))) {
      int size = BytecodeArray::BodyDescriptorWeak::SizeOf(map, object);
      VisitMapPointer(object, object->map_slot());
      BytecodeArray::BodyDescriptorWeak::IterateBody(object, size, this);
      // Aging of bytecode arrays is done on the main thread.
      deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    }
    return 0;
  }

  int VisitJSFunction(Map* map, JSFunction* object) {
    if (!ShouldVisit(object)) return 0;
    int size = JSFunction::BodyDescriptorWeak::SizeOf(map, object);
    VisitMapPointer(object, object->map_slot());
    JSFunction::BodyDescriptorWeak::IterateBody(object, size, this);
    return size;
  }

  int VisitMap(Map* map, Map* object) {
    // TODO(ulan): implement iteration of strong fields.
    deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    return 0;
  }

  int VisitNativeContext(Map* map, Context* object) {
    if (ObjectMarking::IsGrey<AccessMode::ATOMIC>(object,
                                                  marking_state(object))) {
      int size = Context::BodyDescriptorWeak::SizeOf(map, object);
      VisitMapPointer(object, object->map_slot());
      Context::BodyDescriptorWeak::IterateBody(object, size, this);
      // TODO(ulan): implement proper weakness for normalized map cache
      // and remove this bailout.
      deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    }
    return 0;
  }

  int VisitSharedFunctionInfo(Map* map, SharedFunctionInfo* object) {
    if (ObjectMarking::IsGrey<AccessMode::ATOMIC>(object,
                                                  marking_state(object))) {
      int size = SharedFunctionInfo::BodyDescriptorWeak::SizeOf(map, object);
      VisitMapPointer(object, object->map_slot());
      SharedFunctionInfo::BodyDescriptorWeak::IterateBody(object, size, this);
      // Resetting of IC age counter is done on the main thread.
      deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    }
    return 0;
  }

  int VisitTransitionArray(Map* map, TransitionArray* object) {
    // TODO(ulan): implement iteration of strong fields.
    deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    return 0;
  }

  int VisitWeakCell(Map* map, WeakCell* object) {
    // TODO(ulan): implement iteration of strong fields.
    deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    return 0;
  }

  int VisitJSWeakCollection(Map* map, JSWeakCollection* object) {
    // TODO(ulan): implement iteration of strong fields.
    deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
    return 0;
  }

  void MarkObject(HeapObject* object) {
#ifdef THREAD_SANITIZER
    // Perform a dummy acquire load to tell TSAN that there is no data race
    // in mark-bit initialization. See MemoryChunk::Initialize for the
    // corresponding release store.
    MemoryChunk* chunk = MemoryChunk::FromAddress(object->address());
    CHECK_NOT_NULL(chunk->synchronized_heap());
#endif
    if (ObjectMarking::WhiteToGrey<AccessMode::ATOMIC>(object,
                                                       marking_state(object))) {
      deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kShared);
    }
  }

 private:
  // Helper class for collecting in-object slot addresses and values.
  class SlotSnapshottingVisitor final : public ObjectVisitor {
   public:
    explicit SlotSnapshottingVisitor(SlotSnapshot* slot_snapshot)
        : slot_snapshot_(slot_snapshot) {
      slot_snapshot_->clear();
    }

    void VisitPointers(HeapObject* host, Object** start,
                       Object** end) override {
      for (Object** p = start; p < end; p++) {
        Object* object = reinterpret_cast<Object*>(
            base::Relaxed_Load(reinterpret_cast<const base::AtomicWord*>(p)));
        slot_snapshot_->add(p, object);
      }
    }

   private:
    SlotSnapshot* slot_snapshot_;
  };

  const SlotSnapshot& MakeSlotSnapshot(Map* map, HeapObject* object, int size) {
    // TODO(ulan): Iterate only the existing fields and skip slack at the end
    // of the object.
    SlotSnapshottingVisitor visitor(&slot_snapshot_);
    visitor.VisitPointer(object,
                         reinterpret_cast<Object**>(object->map_slot()));
    JSObject::BodyDescriptor::IterateBody(object, size, &visitor);
    return slot_snapshot_;
  }

  MarkingState marking_state(HeapObject* object) const {
    return MarkingState::Internal(object);
  }

  ConcurrentMarkingDeque* deque_;
  SlotSnapshot slot_snapshot_;
};

class ConcurrentMarking::Task : public CancelableTask {
 public:
  Task(Isolate* isolate, ConcurrentMarking* concurrent_marking,
       base::Semaphore* on_finish)
      : CancelableTask(isolate),
        concurrent_marking_(concurrent_marking),
        on_finish_(on_finish) {}

  virtual ~Task() {}

 private:
  // v8::internal::CancelableTask overrides.
  void RunInternal() override {
    concurrent_marking_->Run();
    on_finish_->Signal();
  }

  ConcurrentMarking* concurrent_marking_;
  base::Semaphore* on_finish_;
  DISALLOW_COPY_AND_ASSIGN(Task);
};

ConcurrentMarking::ConcurrentMarking(Heap* heap, ConcurrentMarkingDeque* deque)
    : heap_(heap),
      pending_task_semaphore_(0),
      deque_(deque),
      visitor_(new ConcurrentMarkingVisitor(deque_)),
      is_task_pending_(false) {
  // The runtime flag should be set only if the compile time flag was set.
#ifndef V8_CONCURRENT_MARKING
  CHECK(!FLAG_concurrent_marking);
#endif
}

ConcurrentMarking::~ConcurrentMarking() { delete visitor_; }

void ConcurrentMarking::Run() {
  double time_ms = heap_->MonotonicallyIncreasingTimeInMs();
  size_t bytes_marked = 0;
  base::Mutex* relocation_mutex = heap_->relocation_mutex();
  {
    TimedScope scope(&time_ms);
    while (true) {
      base::LockGuard<base::Mutex> guard(relocation_mutex);
      HeapObject* object = deque_->Pop(MarkingThread::kConcurrent);
      if (object == nullptr) break;
      Address new_space_top = heap_->new_space()->original_top();
      Address new_space_limit = heap_->new_space()->original_limit();
      Address addr = object->address();
      if (new_space_top <= addr && addr < new_space_limit) {
        deque_->Push(object, MarkingThread::kConcurrent, TargetDeque::kBailout);
      } else {
        Map* map = object->synchronized_map();
        bytes_marked += visitor_->Visit(map, object);
      }
    }
  }
  if (FLAG_trace_concurrent_marking) {
    heap_->isolate()->PrintWithTimestamp("concurrently marked %dKB in %.2fms\n",
                                         static_cast<int>(bytes_marked / KB),
                                         time_ms);
  }
}

void ConcurrentMarking::StartTask() {
  if (!FLAG_concurrent_marking) return;
  is_task_pending_ = true;
  V8::GetCurrentPlatform()->CallOnBackgroundThread(
      new Task(heap_->isolate(), this, &pending_task_semaphore_),
      v8::Platform::kShortRunningTask);
}

void ConcurrentMarking::WaitForTaskToComplete() {
  if (!FLAG_concurrent_marking) return;
  pending_task_semaphore_.Wait();
  is_task_pending_ = false;
}

void ConcurrentMarking::EnsureTaskCompleted() {
  if (IsTaskPending()) {
    WaitForTaskToComplete();
  }
}

}  // namespace internal
}  // namespace v8
