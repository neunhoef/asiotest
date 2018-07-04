#ifndef LF_RICHARD_WORKER_FARM
#define LF_RICHARD_WORKER_FARM
#include <boost/lockfree/queue.hpp>
#include "worker_farm.h"


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
    std::atomic<int> lock;

public:
    spin_lock() : lock(0) {}

    uint64_t get()
    {
        uint64_t spins = 0;

        while (!try_lock()) {
          cpu_relax();  
          spins++;
        }

        return spins;
    }

    void release() {
        lock.store(0);
    }

    bool try_lock() {
        int v = lock.exchange(1);

        return v == 0;
    }    
};
#endif



class LockfreeRichardWorkerFarm : public WorkerFarm {

public:
  std::mutex mutex_;
  std::mutex f_mutex_2; // added for testing purpose
  spin_lock f_mutex_;

  struct Sleeper {
    std::condition_variable cond_;
  };

  std::deque<std::shared_ptr<Sleeper>> sleeperQueue_;
  boost::lockfree::queue<Work*>    workQueue_;

  std::atomic<uint32_t> nrThreadsInRun_;
  std::atomic<uint32_t> nrThreadsAwake_;
  std::atomic<uint64_t> num_sleeps;
  bool shouldStop_;
  std::atomic<bool> stopIfFinish_;
  int tick; // guared by f_mutex_
  std::atomic<unsigned> size_;

  size_t maxQueueLen_;

 public:
  LockfreeRichardWorkerFarm(size_t maxQueueLen) 
    : workQueue_(maxQueueLen), nrThreadsInRun_(0), nrThreadsAwake_(0), num_sleeps(0), shouldStop_(false), stopIfFinish_(false), tick(0), maxQueueLen_(maxQueueLen) {
  }

  ~LockfreeRichardWorkerFarm() {
    while (nrThreadsInRun_ > 0) {
      usleep(100);
    }
  }

  bool submit(Work* work) {
    // Returns true if successfully submitted and false if rejected
    
    FUTEX_LOCK;

    // Not directly implemented in lockfree queue -- ignore for testing.
    if (size_ >= maxQueueLen_) {
      FUTEX_UNLOCK;
      return false;
    }

    workQueue_.push(work);
    size_++;

    if (size_ < nrThreadsAwake_) {
      tick = nrThreadsAwake_;
    } else {
      tick--;
    }

    if ((tick <= 0 && nrThreadsAwake_ < nrThreadsInRun_) || nrThreadsAwake_ == 0) {
      std::lock_guard<std::mutex> guard(mutex_);
      if (sleeperQueue_.size() == 0) {
        // THIS HAPPENDS VERY OFTEN!
      } else {
        sleeperQueue_.front()->cond_.notify_one();
        sleeperQueue_.pop_front();
      }
    }

    FUTEX_UNLOCK;
    return true;
  }

  // Arbitrarily many threads can call this to join the farm:
  void run(WorkerStat &stat) {

    nrThreadsInRun_++;
    nrThreadsAwake_++;
    while (true) {
      std::unique_ptr<Work> work = getWork(stat);
      if (work == nullptr || shouldStop_) {
        break ;
      }
      stat.num_work++;
      auto start = std::chrono::high_resolution_clock::now();
      work->doit();
      
      auto end = std::chrono::high_resolution_clock::now();
      stat.work_time += std::chrono::nanoseconds(end - start).count();
    }

    nrThreadsInRun_--;
  }

  // Call this from any thread to stop the work farm, work items will be
  // completed but no new work is begun. This function returns immediately,
  // the destructor waits until all threads running in the run method have
  // left it.
  void stop() {
    shouldStop_ = true;
    FUTEX_LOCK;
    std::lock_guard<std::mutex> guard(mutex_);
    while (sleeperQueue_.size() > 0) {
      sleeperQueue_.front()->cond_.notify_one();
      sleeperQueue_.pop_front();
    }
    FUTEX_UNLOCK;
  }

  void stopWhenDone()
  {
  	stopIfFinish_ = true;
    std::lock_guard<std::mutex> guard(mutex_);
    FUTEX_LOCK;
    while (sleeperQueue_.size() > 0) {
      sleeperQueue_.front()->cond_.notify_one();
      sleeperQueue_.pop_front();
    }
    FUTEX_UNLOCK;
  }

 private:

  std::unique_ptr<Work> getWork(WorkerStat &stat) {

    while (!shouldStop_) { // Wakeup could be spurious!
      FUTEX_LOCK;

      Work *work;
      if (workQueue_.pop(work))
      {
        size_--;
        FUTEX_UNLOCK;
        return std::unique_ptr<Work>(work);
      }
      

      if (stopIfFinish_) {
      	FUTEX_UNLOCK;
      	break ;
      }

      std::unique_lock<std::mutex> guard(mutex_);
      --nrThreadsAwake_;
      // we definitly go to sleep
      stat.num_sleeps++;
      FUTEX_UNLOCK;

      auto sleeper = std::make_shared<Sleeper>();
      sleeperQueue_.push_back(sleeper);  // a copy of the shared_ptr
      sleeper->cond_.wait(guard);
      nrThreadsAwake_++;
    }


    return nullptr;
  }
};

#endif