#ifndef SPIN_LOCK_RICHARD
#define SPIN_LOCK_RICHARD

inline void cpu_relax() {
// TODO use <boost/fiber/detail/cpu_relax.hpp> when available (>1.65.0?)
#if defined(__i386) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(_M_X64)
#if defined _WIN32
  YieldProcessor();
#else
  asm volatile("pause" ::: "memory");
#endif
#else
  static constexpr std::chrono::microseconds us0{0};
  std::this_thread::sleep_for(us0);
#endif
}

#if 1
#define FUTEX_LOCK f_mutex_.get()
#define FUTEX_UNLOCK f_mutex_.release()
#else
#define FUTEX_LOCK f_mutex_2.lock()
#define FUTEX_UNLOCK f_mutex_2.unlock()
#endif

class spin_lock {
    std::atomic_flag flag;

public:
    spin_lock() : flag(ATOMIC_FLAG_INIT) {}

    void get()
    {
        while (!try_lock()) {
          cpu_relax();
        }
    }

    void release() {
        flag.clear();
    }

    bool try_lock() {
        return !flag.test_and_set();
    }
};
#endif
