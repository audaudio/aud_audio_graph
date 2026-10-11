// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// The resources one process shares between its graphs (lifecycle-002,
// ticket 25): the worker threads that run the tasks of the packages - the
// loads, the streaming reads, the frees -, and the sample cache budget with
// the caches the packages register. The workers start with the first graph
// and stop after the last one; the caches and the budget belong to the
// process, since the packages share what they load between graphs. Each
// graph counts the tasks its nodes post, so that it can wait for them when
// it goes without waiting for those of the other graphs.

#ifndef AUD_GRAPH_RESOURCES_HPP
#define AUD_GRAPH_RESOURCES_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "aud_abi.h"
#include "aud_audio_graph.h"
#include "aud_semaphore.hpp"

namespace aud {

// The worker threads of the process.
constexpr uint32_t kWorkerThreads = 2;
// Tasks waiting for a worker.
constexpr uint32_t kTaskQueueCapacity = 4096;
// Caches the packages of one process may register.
constexpr uint32_t kMaxCaches = 16;
// The sample cache budget of a process until the app sets one: 256 MB.
constexpr uint64_t kDefaultCacheBudget = 256ull << 20;

// A bounded lock-free queue for many producers and many consumers (after
// Dmitry Vyukov's bounded MPMC queue): a push neither allocates nor locks,
// so the realtime thread may post; a full queue refuses the push.
template <typename T>
class MpmcQueue {
 public:
  // Creates a queue whose capacity is the next power of two of `capacity`.
  explicit MpmcQueue(size_t capacity) {
    size_t size = 1;
    while (size < capacity) size <<= 1;
    cells_.reset(new Cell[size]);
    mask_ = size - 1;
    for (size_t i = 0; i < size; ++i) {
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  bool push(const T& item) {
    size_t position = enqueue_.load(std::memory_order_relaxed);
    Cell* cell = nullptr;
    for (;;) {
      cell = &cells_[position & mask_];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      const intptr_t difference =
          static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
      if (difference == 0) {
        if (enqueue_.compare_exchange_weak(position, position + 1,
                                           std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < 0) {
        return false;
      } else {
        position = enqueue_.load(std::memory_order_relaxed);
      }
    }
    cell->item = item;
    cell->sequence.store(position + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& item) {
    size_t position = dequeue_.load(std::memory_order_relaxed);
    Cell* cell = nullptr;
    for (;;) {
      cell = &cells_[position & mask_];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      const intptr_t difference = static_cast<intptr_t>(sequence) -
                                  static_cast<intptr_t>(position + 1);
      if (difference == 0) {
        if (dequeue_.compare_exchange_weak(position, position + 1,
                                           std::memory_order_relaxed)) {
          break;
        }
      } else if (difference < 0) {
        return false;
      } else {
        position = dequeue_.load(std::memory_order_relaxed);
      }
    }
    item = cell->item;
    cell->sequence.store(position + mask_ + 1, std::memory_order_release);
    return true;
  }

 private:
  struct Cell {
    std::atomic<size_t> sequence{0};
    T item{};
  };
  std::unique_ptr<Cell[]> cells_;
  size_t mask_ = 0;
  alignas(64) std::atomic<size_t> enqueue_{0};
  alignas(64) std::atomic<size_t> dequeue_{0};
};

// A task in the queue with the counter of the graph that posted it, NULL
// for a task of the process itself.
struct QueuedTask {
  AudTask task{};
  std::atomic<uint32_t>* owner = nullptr;
};

// A cache a package registered.
struct CacheSlot {
  std::atomic<void*> cache{nullptr};
  const AudCacheVTable* vtable = nullptr;
  std::atomic<int64_t> bytes{0};
};

// The resources of the process; one instance that is never destroyed, so
// that no task or cache outlives it at exit.
class Resources {
 public:
  static Resources& get();

  // [control] A graph starts or stops using the resources: the first one
  // starts the workers, the last one drains the tasks and joins them.
  void attach();
  void detach();

  // [any thread] Queues a task; AUD_ERROR_STATE without workers. `owner`,
  // the task counter of the graph that posts it, counts it until it ran.
  int32_t post(const AudTask& task, std::atomic<uint32_t>* owner = nullptr);

  // [control, offline] Waits until no task waits or runs; false on a
  // timeout (0 waits as long as it takes).
  bool waitIdle(uint32_t timeoutMs);

  // [control] Waits until no task that `owner` counts waits or runs; false
  // on a timeout (0 waits as long as it takes).
  bool waitTasks(const std::atomic<uint32_t>& owner, uint32_t timeoutMs);

  // [control] The caches.
  int32_t registerCache(const AudCacheVTable* vtable, void* cache);
  void unregisterCache(void* cache);
  // [any thread] A cache holds `delta` bytes more (or fewer).
  void cacheChanged(void* cache, int64_t delta);

  // [control] The budget of the process in bytes.
  void setBudget(uint64_t bytes);
  uint64_t budget() const { return budget_.load(std::memory_order_acquire); }
  void usage(AudCacheUsage* usage);

  // [any thread] The next id of a load, unique in the process.
  int32_t nextLoadId();

 private:
  Resources();
  void workerLoop();
  bool waitZero(const std::atomic<uint32_t>& count, uint32_t timeoutMs);
  void requestTrim();
  static void trimTask(void* user);
  void trim();

  MpmcQueue<QueuedTask> tasks_{kTaskQueueCapacity};
  AudSemaphore wake_;
  std::vector<std::thread> workers_;
  std::mutex lifecycle_;
  uint32_t attached_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  // Tasks queued or running; waitIdle waits for zero.
  std::atomic<uint32_t> pending_{0};
  std::mutex idleMutex_;
  std::condition_variable idle_;

  // Register, unregister and trim; cacheChanged takes no lock.
  std::mutex cachesMutex_;
  CacheSlot caches_[kMaxCaches];
  std::atomic<int64_t> total_{0};
  std::atomic<uint64_t> budget_{kDefaultCacheBudget};
  // A trim task is queued or runs; a request came since it last looked.
  std::atomic<bool> trimPending_{false};
  std::atomic<bool> trimRequested_{false};
  std::atomic<uint64_t> trims_{0};
  std::atomic<uint64_t> trimmed_{0};
  std::atomic<int32_t> loadIds_{0};
};

}  // namespace aud

#endif  // AUD_GRAPH_RESOURCES_HPP
