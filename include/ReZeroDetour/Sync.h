#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <windows.h>

namespace ReZeroDetour::detail {

    class CriticalSection {
      public:
        CriticalSection() { InitializeCriticalSection(&Cs); }

        ~CriticalSection() { DeleteCriticalSection(&Cs); }

        CriticalSection(const CriticalSection &) = delete;
        CriticalSection &operator=(const CriticalSection &) = delete;

        void lock() noexcept { EnterCriticalSection(&Cs); }

        void unlock() noexcept { LeaveCriticalSection(&Cs); }

      private:
        CRITICAL_SECTION Cs{};
    };

    template <typename T>
    concept LockLike = requires(T &lock) {
        lock.lock();
        lock.unlock();
    };

    template <LockLike T> class LockGuard {
      public:
        explicit LockGuard(T &lock) : Lock(lock) { Lock.lock(); }

        ~LockGuard() { Lock.unlock(); }

        LockGuard(const LockGuard &) = delete;
        LockGuard &operator=(const LockGuard &) = delete;

      private:
        T &Lock;
    };

} // namespace ReZeroDetour::detail
