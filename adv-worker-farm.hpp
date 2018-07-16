#ifndef ADVANCED_WORKER_FARM_H
#define ADVANCED_WORKER_FARM_H
#include <boost/lockfree/queue.hpp>
#include "worker_farm.h"

uint64_t get_tick_count_ns ()
{
  auto now = std::chrono::high_resolution_clock::now();

  return std::chrono::duration_cast<std::chrono::nanoseconds>
    (now.time_since_epoch()).count();
}

class AdvWorkerFarm : public WorkerFarm
{
protected:
  boost::lockfree::queue<Work*> queue_;

  std::mutex mutex_;
  std::condition_variable condition_work_;

  std::atomic<uint64_t> jobs_submitted_, jobs_done_;

  std::atomic<uint64_t> wakeup_queue_length;                          // q1
  std::atomic<uint64_t> wakeup_time_ns_, definitive_wakeup_time_ns_;  // t3, t4

  struct worker_configuration {
    uint64_t queue_retry_cnt;     // t1
    uint64_t sleep_timeout_ms;    // t2

    // initialise with harmless defaults: spin once, sleep forever
    worker_configuration() : queue_retry_cnt(0), sleep_timeout_ms(0) {}
  };

  std::vector<worker_configuration> worker_cfg_;
  std::mutex worker_cfg_mutex_;
  size_t max_worker_cnt, worker_cnt_;

  std::atomic<bool> should_stop_;
  std::atomic<bool> stop_when_done_;

public:
  AdvWorkerFarm(size_t queue_size, size_t max_worker_cnt) :
    queue_(queue_size),
    wakeup_queue_length(5),
    wakeup_time_ns_(1000),
    definitive_wakeup_time_ns_(100000),
    worker_cfg_(max_worker_cnt),
    max_worker_cnt(max_worker_cnt),
    worker_cnt_(0)
  {}


  bool submit(Work *work)
  {
    static thread_local uint64_t last_submit_time_ns;
    bool do_notify = false;

    if (!queue_.push(work)) {
      return false;
    }

    // use memory order release to make sure, pushed item is visible
    uint64_t jobs_submitted = jobs_submitted_.fetch_add(1, std::memory_order_release);
    uint64_t approx_queue_length = jobs_done_ - jobs_submitted;

    uint64_t now_ns = get_tick_count_ns();
    uint64_t sleepy_time_ns = now_ns - last_submit_time_ns;
    last_submit_time_ns = now_ns;

    if (sleepy_time_ns > definitive_wakeup_time_ns_.load(std::memory_order_relaxed)) {
      do_notify = true;

    } else if (sleepy_time_ns > wakeup_time_ns_) {
      if (approx_queue_length > wakeup_queue_length.load(std::memory_order_relaxed)) {
        do_notify = true;
      }
    }



    if (do_notify) {
      condition_work_.notify_one();
    }

    return true;
  }

  virtual void stopWhenDone()
  {
    std::lock_guard<std::mutex> guard(mutex_);
    stop_when_done_ = true;
    condition_work_.notify_all();
  }

  virtual void stop()
  {
    std::lock_guard<std::mutex> guard(mutex_);
    should_stop_ = true;
    condition_work_.notify_all();
  }

  void run(WorkerStat &stat)
  {
    uint64_t id;

    if (!worker_init (id)) {
      return ;
    }

    Work *work;

    while (get_work(id, work, stat)) {
      stat.num_work++;
      auto start = std::chrono::high_resolution_clock::now();
      work->doit();
      auto end = std::chrono::high_resolution_clock::now();
      stat.work_time += std::chrono::nanoseconds(end - start).count();
      jobs_done_.fetch_add(1, std::memory_order_release);
    }
  }

  virtual void run_supervisor() = 0;

private:

  bool worker_init (uint64_t &id)
  {
    std::lock_guard<std::mutex> guard(worker_cfg_mutex_);

    if (worker_cnt_ >= max_worker_cnt) {
      return false;
    }

    id = worker_cnt_++;
    return true;
  }

  bool get_work(uint64_t id, Work *&work, WorkerStat &stat)
  {
    std::cv_status cv_status = std::cv_status::no_timeout;

    while (!should_stop_.load(std::memory_order_relaxed))
    {
      worker_configuration &cfg = worker_cfg_[id];

      uint64_t tries_count = 0;

      while (tries_count < cfg.queue_retry_cnt)
      {
        if (queue_.pop(work)) {
          return true;
        }

        tries_count++;
        if (tries_count < cfg.queue_retry_cnt - 1) {
          cpu_relax();
        }
      }

      // now go to sleep
      std::unique_lock<std::mutex> guard(mutex_);

      if (stop_when_done_ || should_stop_) {
        break ;
      }

      // Only count wakes that are not due to pure timeout
      // i.e. don't count if timeout and now work performed
      if (cv_status != std::cv_status::timeout) {
        stat.num_sleeps++;
      }

      // since a wait for 0 secs does not make any sense, use it to encode indefinite
      // wait time


      if (cfg.sleep_timeout_ms == 0) {
        condition_work_.wait(guard);
        cv_status = std::cv_status::no_timeout;
      } else {
        cv_status = condition_work_.wait_for(guard, std::chrono::milliseconds(cfg.sleep_timeout_ms));
      }
    }

    return false;
  }
};

class AdvStupidWorkerFarm : public AdvWorkerFarm
{
public:
  AdvStupidWorkerFarm(size_t queue_size, size_t max_worker_cnt) :
    AdvWorkerFarm(queue_size, max_worker_cnt) {}

  void run_supervisor()
  {
    /*
    I therefore propose that we build our first shot at this with t1=100 (times round
    <pause, get job> when there is no job) t2=100 milliseconds (sleep on condition variable)
    t3=300 microseconds (maximal frequency of wakeup in submitter) t4=61.8033 milliseconds
    (minimal frequency of wakeup in submitter) q1=5 (queue size for frequent wakeup
    in submitter) and see how that behaves.  The supervisor is essentially null - it wakes
    up,  looks perhaps at the numbers of jobs submitted and processed, updates its
    statistics only to ignore everything and set t1[w] t2[w] t3 t4 q1 to these constants!
    */

    // Initialise everything
    wakeup_queue_length = 5;
    wakeup_time_ns_ = 300000;
    definitive_wakeup_time_ns_ = 61803300;

    for (auto &cfg : worker_cfg_) {
      cfg.queue_retry_cnt = 100;
      cfg.sleep_timeout_ms = 100;
    }

    // and do nothing
  }
};


#endif
