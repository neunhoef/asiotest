#ifndef WORKER_FARM_FUTEX_H
#define WORKER_FARM_FUTEX_H
#include <boost/lockfree/queue.hpp>
#include "worker_farm.h"

class Futex {
public:
  Futex() : _val() {}
  Futex(int val) : _val(val) {}

  std::atomic<int>& value() { return _val; }

  void wait(int expectedValue);

  void notifyOne();

  void notifyAll();
private:
  std::atomic<int> _val;
};

#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>

namespace {
  static int futex(std::atomic<int>* uaddr, int futex_op, int val,
    const struct timespec *timeout, int *uaddr2, int val3)
  {
    return syscall(SYS_futex, reinterpret_cast<int*>(uaddr), futex_op, val, timeout, uaddr, val3);
  }
}

void Futex::wait(int expectedValue) {
  futex(&_val, FUTEX_WAIT_PRIVATE, expectedValue, nullptr, nullptr, 0);
}

void Futex::notifyOne() {
  futex(&_val, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void Futex::notifyAll() {
  futex(&_val, FUTEX_WAKE_PRIVATE, std::numeric_limits<int>::max(), nullptr, nullptr, 0);
}
#endif

class FutexWorkerFarm : public WorkerFarm {
  boost::lockfree::queue<Work*>    workQueue_;
  Futex size_;

  std::atomic<bool> shouldStop_;
  std::atomic<bool> stopWhenDone_;
  std::atomic<unsigned> waiting_;
  std::atomic<unsigned> nrThreadsInRun_;

  int maxQueueLen_; // has to be int because futexes only support 32-bit int

 public:
  FutexWorkerFarm(int maxQueueLen) 
    : workQueue_(maxQueueLen), size_(0), shouldStop_(false), waiting_(0), nrThreadsInRun_(0), maxQueueLen_(maxQueueLen) {
  }

  ~FutexWorkerFarm() {
    while (nrThreadsInRun_ > 0) {
      usleep(100);
    }
  }

  bool submit(Work* work) {
    // Returns true if successfully submitted and false if rejected  
    if (size_.value().load(std::memory_order_relaxed) + 1 >= maxQueueLen_)
      return false;
      
    if (!workQueue_.push(work))
      return false;
      
    auto pendingJobs = size_.value().fetch_add(1, std::memory_order_seq_cst);
    
    // (1) - this seq-cst load ensures a total order with the seq-cst fetch-add (2) 
    if (waiting_.load(std::memory_order_seq_cst) == nrThreadsInRun_.load(std::memory_order_relaxed) || pendingJobs >= 20)
      size_.notifyOne();
    return true;
  }

  // Arbitrarily many threads can call this to join the farm:
  void run(WorkerStat &stat) {
    nrThreadsInRun_++;
    while (true) {
      Work* work = getWork();
      if (work == nullptr || shouldStop_.load(std::memory_order_relaxed)) {
        delete work;
        break;
      }
      stat.num_work++;
      auto start = std::chrono::high_resolution_clock::now();
      work->doit();
      auto end = std::chrono::high_resolution_clock::now();
      stat.work_time += std::chrono::nanoseconds(end - start).count();
      delete work;
    }
    nrThreadsInRun_--;
  }

  // Call this from any thread to stop the work farm, work items will be
  // completed but no new work is begun. This function returns immediately,
  // the destructor waits until all threads running in the run method have
  // left it.
  void stop() {
    shouldStop_.store(true, std::memory_order_relaxed);
    size_.notifyAll();
  }

  void stopWhenDone() {
  	stopWhenDone_.store(true, std::memory_order_relaxed);
  	size_.notifyAll();	// fix me
  }

 private:
  Work* getWork() {
    while (!shouldStop_.load(std::memory_order_relaxed))
    {
      const int max_tries = 3; // try a few times to fetch a job before going to sleep
      for (int i = 0; i < max_tries; ++i) {
        Work* work;
        if (workQueue_.pop(work)) {
          if (size_.value().fetch_add(-1, std::memory_order_relaxed) >= 10)
            size_.notifyOne();
          return work;
        }
        
        if (i < max_tries - 1)
          std::this_thread::yield();
      }

      if (stopWhenDone_)
      {
      	break ;
      }
      
      // (2) - this seq-cst fetch-add ensures a total order with the seq-cst load (1)
      waiting_.fetch_add(1, std::memory_order_seq_cst);
      size_.wait(0);
      waiting_.fetch_add(-1, std::memory_order_relaxed);
    }
    return nullptr;
  }
 
};

#endif