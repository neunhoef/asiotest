#ifndef RICHARD_WORKER_FARM
#define RICHARD_WORKER_FARM
#include "worker_farm.h"

#include "spin_lock.hpp"

class RichardWorkerFarm : public WorkerFarm {

public:
  std::mutex mutex_;
  std::mutex f_mutex_2; // added for testing purpose
  spin_lock f_mutex_;

  struct Sleeper {
    std::condition_variable cond_;
  };

  std::deque<std::shared_ptr<Sleeper>> sleeperQueue_;
  std::deque<std::unique_ptr<Work>>    workQueue_;

  std::atomic<uint32_t> nrThreadsInRun_;
  std::atomic<uint32_t> nrThreadsAwake_;
  std::atomic<uint64_t> num_sleeps;
  bool shouldStop_;
  std::atomic<bool> stopIfFinish_;
  int tick; // guared by f_mutex_

  size_t maxQueueLen_;

 public:
  RichardWorkerFarm(size_t maxQueueLen)
    : nrThreadsInRun_(0), nrThreadsAwake_(0), num_sleeps(0), shouldStop_(false), stopIfFinish_(false), tick(0), maxQueueLen_(maxQueueLen) {
  }

  ~RichardWorkerFarm() {
    while (nrThreadsInRun_ > 0) {
      usleep(100);
    }
  }

  bool submit(Work* work) {
    // Returns true if successfully submitted and false if rejected

    FUTEX_LOCK;

    if (workQueue_.size() >= maxQueueLen_) {
      FUTEX_UNLOCK;
      return false;
    }

    workQueue_.emplace_back(work);

    if (workQueue_.size() < nrThreadsAwake_) {
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

      if (workQueue_.size() != 0) {
        std::unique_ptr<Work> work(std::move(workQueue_.front()));
        workQueue_.pop_front();
        FUTEX_UNLOCK;
        return work;
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
