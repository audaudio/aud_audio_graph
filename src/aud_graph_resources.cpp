// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

#include "aud_graph_resources.hpp"

#include <algorithm>
#include <chrono>

namespace aud {

Resources& Resources::get() {
  // Never destroyed: a worker or a package's cache may still run at exit.
  static Resources* resources = new Resources();
  return *resources;
}

Resources::Resources() = default;

// ............................................................................
// The workers

void Resources::attach() {
  std::lock_guard<std::mutex> lock(lifecycle_);
  attached_ += 1;
  if (attached_ > 1) return;
  stopping_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  for (uint32_t i = 0; i < kWorkerThreads; ++i) {
    workers_.emplace_back([this] { workerLoop(); });
  }
}

void Resources::detach() {
  std::lock_guard<std::mutex> lock(lifecycle_);
  if (attached_ == 0) return;
  attached_ -= 1;
  if (attached_ > 0) return;
  // The workers run what is queued - the packages free memory in tasks -
  // and leave when the queue is empty.
  stopping_.store(true, std::memory_order_release);
  for (size_t i = 0; i < workers_.size(); ++i) wake_.post();
  for (std::thread& worker : workers_) worker.join();
  workers_.clear();
  running_.store(false, std::memory_order_release);
}

int32_t Resources::post(const AudTask& task) {
  if (task.struct_size < sizeof(AudTask) || task.run == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  if (!running_.load(std::memory_order_acquire)) return AUD_ERROR_STATE;
  pending_.fetch_add(1, std::memory_order_acq_rel);
  if (!tasks_.push(task)) {
    // No notification here - the caller may be the realtime thread; a
    // waiter of waitIdle looks at the count every millisecond.
    pending_.fetch_sub(1, std::memory_order_acq_rel);
    return AUD_ERROR_QUEUE_FULL;
  }
  wake_.post();
  return AUD_OK;
}

void Resources::workerLoop() {
  for (;;) {
    AudTask task{};
    if (tasks_.pop(task)) {
      task.run(task.user);
      if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(idleMutex_);
        idle_.notify_all();
      }
      continue;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      // Another worker may still post the last tasks; leave only when the
      // queue stayed empty and nothing runs.
      if (pending_.load(std::memory_order_acquire) == 0) return;
      std::this_thread::yield();
      continue;
    }
    wake_.wait();
  }
}

bool Resources::waitIdle(uint32_t timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::unique_lock<std::mutex> lock(idleMutex_);
  while (pending_.load(std::memory_order_acquire) != 0) {
    if (timeoutMs != 0 && std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    // The last task notifies; a refused post does not, so look again
    // every millisecond.
    idle_.wait_for(lock, std::chrono::milliseconds(1));
  }
  return true;
}

int32_t Resources::nextLoadId() {
  int32_t id = loadIds_.fetch_add(1, std::memory_order_relaxed) + 1;
  // Ids stay positive; after 2^31 loads they start again at 1.
  if (id <= 0) {
    loadIds_.store(1, std::memory_order_relaxed);
    id = 1;
  }
  return id;
}

// ............................................................................
// The caches and the budget

int32_t Resources::registerCache(const AudCacheVTable* vtable, void* cache) {
  if (vtable == nullptr || cache == nullptr ||
      vtable->struct_size < sizeof(AudCacheVTable) || vtable->trim == nullptr) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(cachesMutex_);
  for (CacheSlot& slot : caches_) {
    if (slot.cache.load(std::memory_order_acquire) == cache) return AUD_OK;
  }
  for (CacheSlot& slot : caches_) {
    if (slot.cache.load(std::memory_order_acquire) != nullptr) continue;
    // The vtable is set before the cache pointer publishes the slot.
    slot.vtable = vtable;
    slot.bytes.store(0, std::memory_order_relaxed);
    slot.cache.store(cache, std::memory_order_release);
    return AUD_OK;
  }
  return AUD_ERROR_CAPACITY;
}

// Waits for a trim that runs, so that the cache can go away afterwards.
void Resources::unregisterCache(void* cache) {
  std::lock_guard<std::mutex> lock(cachesMutex_);
  for (CacheSlot& slot : caches_) {
    if (slot.cache.load(std::memory_order_acquire) != cache) continue;
    total_.fetch_sub(slot.bytes.exchange(0, std::memory_order_acq_rel),
                     std::memory_order_acq_rel);
    slot.cache.store(nullptr, std::memory_order_release);
    return;
  }
}

void Resources::cacheChanged(void* cache, int64_t delta) {
  for (CacheSlot& slot : caches_) {
    if (slot.cache.load(std::memory_order_acquire) != cache) continue;
    slot.bytes.fetch_add(delta, std::memory_order_acq_rel);
    const int64_t total =
        total_.fetch_add(delta, std::memory_order_acq_rel) + delta;
    if (delta > 0 && total > static_cast<int64_t>(budget())) requestTrim();
    return;
  }
}

// A smaller budget trims at once, on the calling thread: without a graph
// there is no worker to do it.
void Resources::setBudget(uint64_t bytes) {
  budget_.store(bytes, std::memory_order_release);
  if (total_.load(std::memory_order_acquire) > static_cast<int64_t>(bytes)) trim();
}

void Resources::usage(AudCacheUsage* usage) {
  uint32_t caches = 0;
  for (CacheSlot& slot : caches_) {
    if (slot.cache.load(std::memory_order_acquire) != nullptr) caches += 1;
  }
  usage->caches = caches;
  usage->budget = budget();
  usage->bytes = total_.load(std::memory_order_acquire);
  usage->trims = trims_.load(std::memory_order_relaxed);
  usage->trimmed = trimmed_.load(std::memory_order_relaxed);
  usage->workers = running_.load(std::memory_order_acquire)
                       ? static_cast<uint32_t>(kWorkerThreads)
                       : 0;
  usage->tasks = pending_.load(std::memory_order_acquire);
}

// One trim at a time, on a worker; from the realtime thread too, since the
// request only posts. A request while a trim runs is not lost: the task
// trims again for it.
void Resources::requestTrim() {
  trimRequested_.store(true);
  if (trimPending_.exchange(true)) return;
  AudTask task{sizeof(AudTask), 0, &Resources::trimTask, this};
  if (post(task) != AUD_OK) trimPending_.store(false);
}

// The flags are sequentially consistent: either the task sees a request
// that came after its last look, or the request sees the task gone and
// posts a new one.
void Resources::trimTask(void* user) {
  auto* self = static_cast<Resources*>(user);
  for (;;) {
    while (self->trimRequested_.exchange(false)) self->trim();
    self->trimPending_.store(false);
    if (!self->trimRequested_.load() || self->trimPending_.exchange(true)) return;
  }
}

// Trims the largest caches first until the process fits its budget or no
// cache frees anything more. What a trim returns is counted off here; the
// cache reports only its other changes through cacheChanged.
void Resources::trim() {
  std::lock_guard<std::mutex> lock(cachesMutex_);
  for (;;) {
    const int64_t over =
        total_.load(std::memory_order_acquire) - static_cast<int64_t>(budget());
    if (over <= 0) break;
    CacheSlot* order[kMaxCaches];
    uint32_t count = 0;
    for (CacheSlot& slot : caches_) {
      if (slot.cache.load(std::memory_order_acquire) != nullptr) {
        order[count++] = &slot;
      }
    }
    std::sort(order, order + count, [](const CacheSlot* a, const CacheSlot* b) {
      return a->bytes.load(std::memory_order_relaxed) >
             b->bytes.load(std::memory_order_relaxed);
    });
    uint64_t freed = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const int64_t remaining = total_.load(std::memory_order_acquire) -
                                static_cast<int64_t>(budget());
      if (remaining <= 0) break;
      void* cache = order[i]->cache.load(std::memory_order_acquire);
      if (cache == nullptr) continue;
      const uint64_t bytes =
          order[i]->vtable->trim(cache, static_cast<uint64_t>(remaining));
      order[i]->bytes.fetch_sub(static_cast<int64_t>(bytes),
                                std::memory_order_acq_rel);
      total_.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_acq_rel);
      trims_.fetch_add(1, std::memory_order_relaxed);
      trimmed_.fetch_add(bytes, std::memory_order_relaxed);
      freed += bytes;
    }
    if (freed == 0) break;
  }
}

}  // namespace aud

// ############################################################################
// The C API of the resources

AUD_EXPORT int32_t aud_cache_set_budget(uint64_t bytes) {
  aud::Resources::get().setBudget(bytes);
  return AUD_OK;
}

AUD_EXPORT uint64_t aud_cache_budget(void) {
  return aud::Resources::get().budget();
}

AUD_EXPORT int32_t aud_cache_usage(AudCacheUsage* usage) {
  if (usage == nullptr || usage->struct_size < sizeof(AudCacheUsage)) {
    return AUD_ERROR_INVALID_ARGUMENT;
  }
  aud::Resources::get().usage(usage);
  return AUD_OK;
}

AUD_EXPORT int32_t aud_tasks_wait_idle(uint32_t timeout_ms) {
  return aud::Resources::get().waitIdle(timeout_ms) ? AUD_OK : AUD_PENDING;
}
