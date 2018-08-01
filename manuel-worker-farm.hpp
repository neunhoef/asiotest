#ifndef MANUEL_WORKER_FARM_H
#define MANUEL_WORKER_FARM_H
#include <list>
#include <random>
#include <boost/lockfree/queue.hpp>
#include "worker_farm.h"
#include "spin_lock.hpp"

/* As previously discussed, we don't want to spent too much CPU cycles spinning
 * or waking up threads. The idea of this worker farm is to let the threads with
 * lower ids do most of the work. All threads sleep only for a limited amount of
 * time. The timespan for the sleep is randomized and depends on the thread id.
 * i.e., threads with higher ids spin less and sleep longer.
 * The implementation uses individual condition variables for the threads and a
 * custom queueing mechanism that prefers to wake threads with lower ids.
 */
class ManuelWorkerFarm : public WorkerFarm {
public:
  ManuelWorkerFarm(uint32_t maxWorkerThreads, uint32_t maxQueueLen) :
    maxQueueLen_(maxQueueLen),
    jobQueue_(maxQueueLen),
    counters_(0),
    shouldStop_(false),
    stopWhenDone_(false),
    last_coordination_(Clock::now())
  {
    controlBlocks_.resize(maxWorkerThreads);
    uint32_t idx = 0;
    for (auto& cb : controlBlocks_) {
      cb.id = idx++;
      cb.state = WorkerState::inactive;
      cb.rand.seed(cb.id);
    }
  }

  ManuelWorkerFarm() = delete;
  ManuelWorkerFarm(const ManuelWorkerFarm&) = delete;
  ManuelWorkerFarm(ManuelWorkerFarm&&) = delete;

  virtual bool submit(Work* work) override {
    if (!jobQueue_.push(work)) {
      return false;
    }

    // (1) - this release-fetch-add synchronizes with the release-fetch-sub (2)
    auto counters = counters_.fetch_add(QUEUED_JOB_INC, std::memory_order_release);
    coordinateWorkers(counters);
    return true;
  }

  virtual void stopWhenDone() override {
    stopWhenDone_.store(true, std::memory_order_relaxed);
    wakeWorkers(totalWorkers(counters_.load(std::memory_order_relaxed)));
  }

  virtual void stop() override {
    shouldStop_.store(true, std::memory_order_relaxed);
    wakeWorkers(totalWorkers(counters_.load(std::memory_order_relaxed)));
  }

  virtual void run(WorkerStat &stat) override {
    auto controlBlock = allocateControlBlock();
    if (controlBlock) {
      doRun(*controlBlock, stat);
      releaseControlBlock(*controlBlock);
    }
  }

private:
  static constexpr uint32_t min_coordination_timespan_ms = 20;

  using Clock = std::chrono::high_resolution_clock;

  enum class WorkerState {
    inactive,
    running,
    sleeping
  };

  struct ControlBlock {
    uint32_t id;
    std::atomic<WorkerState> state;
    std::mutex mutex;
    std::condition_variable bell;
    char padding[40];
    std::mt19937 rand;

    ControlBlock(): id(), state(WorkerState::inactive), mutex(), bell() {}
    ControlBlock(const ControlBlock&) = delete;
    ControlBlock(ControlBlock&&) {
      throw std::runtime_error("ControlBlock must not be moved");
    }
  };

  ControlBlock* allocateControlBlock() {
    for (auto& cb : controlBlocks_) {
      auto state = cb.state.load(std::memory_order_relaxed);
      if (state == WorkerState::inactive &&
          cb.state.compare_exchange_strong(state, WorkerState::running, std::memory_order_relaxed)) {
        counters_.fetch_add(ACTIVE_WORKER_INC + TOTAL_WORKER_INC, std::memory_order_relaxed);
        return &cb;
      }
    }
    return nullptr;
  }

  void releaseControlBlock(ControlBlock& controlBlock) {
    controlBlock.state.store(WorkerState::inactive, std::memory_order_relaxed);
    counters_.fetch_sub(ACTIVE_WORKER_INC + TOTAL_WORKER_INC, std::memory_order_relaxed);
  }

  void coordinateWorkers(uint64_t counters) {
    const auto active = activeWorkers(counters);
    const auto total = totalWorkers(counters);
    if (active < total) {
      if (active == 0 || hasMinCoordinationTimeSpanPassed()) {
        if (coordination_lock_.try_lock()) {
          auto additionalWorkers = queuedJobs(counters) / (active + 1);
          additionalWorkers = std::min(additionalWorkers, total - active);
          wakeWorkers(additionalWorkers);

          last_coordination_.store(Clock::now(), std::memory_order_relaxed);
          coordination_lock_.release();
        }
      }
    }
  }

  bool hasMinCoordinationTimeSpanPassed() {
    auto diff = Clock::now() - last_coordination_.load(std::memory_order_relaxed);
    return std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() >= min_coordination_timespan_ms;
  }

  void wakeWorkers(uint32_t cnt) {
    for (uint32_t i = 0; i < controlBlocks_.size() && cnt > 0; ++i) {
      ControlBlock& cb = controlBlocks_[i];
      if (cb.state.load(std::memory_order_relaxed) == WorkerState::sleeping) {
        std::unique_lock<std::mutex> lock(cb.mutex, std::defer_lock_t());
        if (cb.id == 0) {
          // If want to wake the first thread, acquire the mutex to avoid a race.
          // Don't bother about the mutex with other threads.
          lock.lock();
        }
        cb.bell.notify_one();
        --cnt;
      }
    }
  }

  void doRun(ControlBlock& controlBlock, WorkerStat& stat) {
    while (true) {
      std::unique_ptr<Work> work(getWork(controlBlock, stat));
      if (work == nullptr || shouldStop_.load(std::memory_order_relaxed)) {
        break;
      }
      stat.num_work++;
      auto start = std::chrono::high_resolution_clock::now();
      work->doit();
      auto end = std::chrono::high_resolution_clock::now();
      stat.work_time += std::chrono::nanoseconds(end - start).count();
    }
  }

  Work* getWork(ControlBlock& controlBlock, WorkerStat& stat) {
    while (!shouldStop_.load(std::memory_order_relaxed))
    {
      const int max_tries = (512 >> controlBlock.id) + 3; // try a few times to fetch a job before going to sleep
      for (int i = 0; i <= max_tries; ++i) {
        Work* work;
        if (jobQueue_.pop(work)) {
          counters_.fetch_sub(QUEUED_JOB_INC, std::memory_order_relaxed);
          return work;
        }

        if (i < max_tries - 1)
          cpu_relax();
      }

      if (stopWhenDone_.load(std::memory_order_relaxed)) {
      	break ;
      }

      std::unique_lock<std::mutex> guard(controlBlock.mutex);
      auto counters = counters_.fetch_sub(ACTIVE_WORKER_INC, std::memory_order_acquire);
      if (queuedJobs(counters) > 0) {
        counters_.fetch_add(ACTIVE_WORKER_INC, std::memory_order_relaxed);
        if (controlBlock.id == 0) {
          std::cout<<"Continue: "<< queuedJobs(counters)<<std::endl;
        }
        continue;
      }

      stat.num_sleeps++;
      controlBlock.state.store(WorkerState::sleeping, std::memory_order_relaxed);
      const uint32_t sleepTime = 10 * (controlBlock.id + 1);
      auto start = std::chrono::high_resolution_clock::now();
      controlBlock.bell.wait_for(guard, std::chrono::milliseconds(sleepTime + (controlBlock.rand() % sleepTime)));
      auto end = std::chrono::high_resolution_clock::now();
      stat.sleep_time += std::chrono::nanoseconds(end - start).count();

      controlBlock.state.store(WorkerState::running, std::memory_order_relaxed);
      counters_.fetch_add(ACTIVE_WORKER_INC, std::memory_order_relaxed);
    }
    return nullptr;
  }

  static uint32_t activeWorkers(uint64_t counters) { return static_cast<uint32_t>(counters & WORKER_MASK); }
  static uint32_t totalWorkers(uint64_t counters) { return static_cast<uint32_t>((counters >> TOTAL_WORKER_SHIFT) & WORKER_MASK); }
  static uint32_t queuedJobs(uint64_t counters) { return static_cast<uint32_t>(counters >> QUEUED_JOB_SHIFT); }

  uint32_t maxQueueLen_;
  std::vector<ControlBlock> controlBlocks_;

  boost::lockfree::queue<Work*> jobQueue_;

  static constexpr uint64_t QUEUED_JOB_SHIFT = 32;
  static constexpr uint64_t QUEUED_JOB_INC = 1ul << QUEUED_JOB_SHIFT;
  static constexpr uint64_t TOTAL_WORKER_SHIFT = 16;
  static constexpr uint64_t TOTAL_WORKER_INC = 1 << TOTAL_WORKER_SHIFT;
  static constexpr uint64_t ACTIVE_WORKER_INC = 1;
  static constexpr uint64_t WORKER_MASK = TOTAL_WORKER_SHIFT - 1;

  std::atomic<uint64_t> counters_;

  std::atomic<bool> shouldStop_;
  std::atomic<bool> stopWhenDone_;

  char padding[64];

  spin_lock coordination_lock_;
  std::atomic<Clock::time_point> last_coordination_;
};

#endif
