#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMUTEX_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMUTEX_HPP

#include <atomic>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

// A mutex for short sections on hot paths. std::mutex and std::shared_mutex are winpthreads objects
// with the MinGW toolchain: taking and releasing one that nobody else wants costs a call into that
// library each way (a shared_mutex more), and the draw path did it some twenty times per draw, 6%
// of its preparing thread. This one is a compare-and-swap each way when it is free; a thread that
// finds it taken polls briefly and then sleeps on the word (std::atomic's wait).
//
// It has the members std::lock_guard, std::unique_lock and std::shared_lock need. A shared lock is
// an exclusive one: the sections it is meant for are a table lookup long.
class HostMutex {
public:
    HostMutex() = default;
    HostMutex(const HostMutex&) = delete;
    HostMutex& operator=(const HostMutex&) = delete;

    void lock() {
        std::uint32_t expected = Free;
        if (state.compare_exchange_strong(expected, Taken, std::memory_order_acquire, std::memory_order_relaxed)) return;
        lockContended();
    }

    bool try_lock() {
        std::uint32_t expected = Free;
        return state.compare_exchange_strong(expected, Taken, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void unlock() {
        if (state.exchange(Free, std::memory_order_release) == Awaited) state.notify_one();
    }

    void lock_shared() { lock(); }
    bool try_lock_shared() { return try_lock(); }
    void unlock_shared() { unlock(); }

private:
    static constexpr std::uint32_t Free = 0, Taken = 1, Awaited = 2;

    void lockContended() {
        for (int spin = 0; spin < 200; ++spin) {
            std::uint32_t expected = Free;
            if (state.load(std::memory_order_relaxed) == Free && state.compare_exchange_weak(expected, Taken, std::memory_order_acquire, std::memory_order_relaxed)) return;
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        }
        // Taken with a waiter marked: the holder wakes one sleeper when it releases, and that one
        // takes the mutex with the mark again, since others may still sleep.
        while (state.exchange(Awaited, std::memory_order_acquire) != Free) state.wait(Awaited, std::memory_order_relaxed);
    }

    std::atomic<std::uint32_t> state{Free};
};

#endif
