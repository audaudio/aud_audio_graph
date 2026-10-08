// @license
// Copyright (c) Audanika. All Rights Reserved.
//
// Use of this source code is governed by terms that can be
// found in the LICENSE file in the root of this package.

// A counting semaphore the realtime thread may post (decision interop-002):
// the post never blocks and takes no lock, so the audio thread can wake
// the notification thread. Header only, one implementation per platform;
// std::counting_semaphore needs C++20 and the packages build as C++17.

#ifndef AUD_SEMAPHORE_HPP
#define AUD_SEMAPHORE_HPP

#if _WIN32
#include <windows.h>
#elif __APPLE__
#include <dispatch/dispatch.h>
#else
#include <errno.h>
#include <semaphore.h>
#endif

class AudSemaphore {
 public:
  AudSemaphore() {
#if _WIN32
    handle_ = CreateSemaphoreA(nullptr, 0, 0x7fffffff, nullptr);
#elif __APPLE__
    handle_ = dispatch_semaphore_create(0);
#else
    sem_init(&handle_, 0, 0);
#endif
  }

  ~AudSemaphore() {
#if _WIN32
    CloseHandle(handle_);
#elif __APPLE__
    // A dispatch semaphore must not be released below its initial count.
    while (dispatch_semaphore_wait(handle_, DISPATCH_TIME_NOW) == 0) {
    }
    dispatch_release(handle_);
#else
    sem_destroy(&handle_);
#endif
  }

  AudSemaphore(const AudSemaphore&) = delete;
  AudSemaphore& operator=(const AudSemaphore&) = delete;

  // [realtime] Increments the count and wakes a waiter; never blocks.
  void post() {
#if _WIN32
    ReleaseSemaphore(handle_, 1, nullptr);
#elif __APPLE__
    dispatch_semaphore_signal(handle_);
#else
    sem_post(&handle_);
#endif
  }

  // Waits until the count is positive and decrements it.
  void wait() {
#if _WIN32
    WaitForSingleObject(handle_, INFINITE);
#elif __APPLE__
    dispatch_semaphore_wait(handle_, DISPATCH_TIME_FOREVER);
#else
    while (sem_wait(&handle_) != 0 && errno == EINTR) {
    }
#endif
  }

 private:
#if _WIN32
  HANDLE handle_;
#elif __APPLE__
  dispatch_semaphore_t handle_;
#else
  sem_t handle_;
#endif
};

#endif  // AUD_SEMAPHORE_HPP
