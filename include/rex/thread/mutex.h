/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace rex::thread {

// The global critical region mutex singleton.
// This must guard any operation that may suspend threads or be sensitive to
// being suspended such as global table locks and such.
// To prevent deadlocks this should be the first lock acquired and be held
// for the entire duration of the critical region (longer than any other lock).
//
// As a general rule if some code can only be accessed from the guest you can
// guard it with only the global critical region and be assured nothing else
// will touch it. If it will be accessed from non-guest threads you may need
// some additional protection.
//
// You can think of this as disabling interrupts in the guest. The thread in the
// global critical region has exclusive access to the entire system and cannot
// be preempted. This also means that all activity done while in the critical
// region must be extremely fast (no IO!), as it has the chance to block any
// other thread until its done.
//
// For example, in the following situation thread 1 will not be able to suspend
// thread 0 until it has exited its critical region, preventing it from being
// suspended while holding the table lock:
//   [thread 0]:
//     DoKernelStuff():
//       auto global_lock = global_critical_region_.Acquire();
//       std::lock_guard<std::mutex> table_lock(table_mutex_);
//       table_->InsertStuff();
//   [thread 1]:
//     MySuspendThread():
//       auto global_lock = global_critical_region_.Acquire();
//       ::SuspendThread(thread0);
//
// To use the region it's strongly recommended that you keep an instance near
// the data requiring it. This makes it clear to those reading that the data
// is protected by the global critical region. For example:
// class MyType {
//   // Implies my_list_ is protected:
//   xe::global_critical_region global_critical_region_;
//   std::list<...> my_list_;
// };
class global_critical_region {
 public:
  static std::recursive_mutex& mutex();

  // Guest "interrupts off" (recompiled mtmsrd / spinlock sequences). The sections are a handful
  // of instructions, so a contended lock spins briefly before it sleeps in the kernel: on Android
  // every futex sleep and wake costs far more than the section itself, and the game's main thread
  // spent over a third of its time blocked here.
  static void EnterGuest() {
    auto& m = mutex();
    for (int i = 0; i < 2000; ++i) {
      if (m.try_lock()) {
        ++guest_depth_;
        return;
      }
      CpuRelax();
    }
    m.lock();
    ++guest_depth_;
  }
  static void LeaveGuest() {
    --guest_depth_;
    mutex().unlock();
  }
  // Whether this thread is inside a guest critical section (the guest's mfmsr EE bit). Only the
  // owning thread can be inside, so no lock is needed to answer.
  static bool GuestHeldByThisThread() { return guest_depth_ != 0; }
  static inline void CpuRelax() {
#if defined(__aarch64__) || defined(_M_ARM64)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
  }
  static inline thread_local int guest_depth_ = 0;

  // Acquires a lock on the global critical section.
  // Use this when keeping an instance is not possible. Otherwise, prefer
  // to keep an instance of global_critical_region near the members requiring
  // it to keep things readable.
  static std::unique_lock<std::recursive_mutex> AcquireDirect() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }

  // Acquires a lock on the global critical section.
  inline std::unique_lock<std::recursive_mutex> Acquire() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }

  // Acquires a deferred lock on the global critical section.
  inline std::unique_lock<std::recursive_mutex> AcquireDeferred() {
    return std::unique_lock<std::recursive_mutex>(mutex(), std::defer_lock);
  }

  // Tries to acquire a lock on the glboal critical section.
  // Check owns_lock() to see if the lock was successfully acquired.
  inline std::unique_lock<std::recursive_mutex> TryAcquire() {
    return std::unique_lock<std::recursive_mutex>(mutex(), std::try_to_lock);
  }
};

// A guest spin-wait iteration (recompiled cctpl). The first iterations of a wait only hint the
// CPU, so short waits stay instant; a thread that keeps spinning sleeps 50 us per iteration
// instead of burning a core (on phones that heat throttles every core). A wait counts as over
// when the thread hasn't paused for 200 us.
inline void GuestSpinPause() {
  using clock = std::chrono::steady_clock;
  static thread_local clock::time_point last;
  static thread_local uint32_t run = 0;
  const auto now = clock::now();
  if (now - last > std::chrono::microseconds(200)) run = 0;
  last = now;
  if (++run <= 64) {
    for (int i = 0; i < 8; ++i) global_critical_region::CpuRelax();
    return;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(50));
  last = clock::now();
}

}  // namespace rex::thread
