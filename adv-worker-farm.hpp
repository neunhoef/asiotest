#ifndef ADVANCED_WORKER_FARM_H
#define ADVANCED_WORKER_FARM_H
#include <list>
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
  size_t max_worker_cnt_;

  std::atomic<size_t> worker_cnt_;

  std::atomic<bool> should_stop_;
  std::atomic<bool> stop_when_done_;

public:
  AdvWorkerFarm(size_t queue_size, size_t max_worker_cnt) :
    queue_(queue_size),
    jobs_submitted_(0),
    jobs_done_(0),
    wakeup_queue_length(5),
    wakeup_time_ns_(1000),
    definitive_wakeup_time_ns_(100000),
    worker_cfg_(max_worker_cnt),
    max_worker_cnt_(max_worker_cnt),
    worker_cnt_(0),
    should_stop_(false),
    stop_when_done_(false)
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

  virtual void run(WorkerStat &stat)
  {
    uint64_t id;

    if (!worker_init (id)) {
      return ;
    }

    Work *work;

    while (get_work(id, work, stat)) {
      work->doit();
      jobs_done_.fetch_add(1, std::memory_order_release);

      delete work;
    }
  }

  virtual void run_supervisor() = 0;

protected:
  virtual bool worker_init (uint64_t &id)
  {
    std::lock_guard<std::mutex> guard(worker_cfg_mutex_);

    if (worker_cnt_ >= max_worker_cnt_) {
      return false;
    }

    id = worker_cnt_++;
    return true;
  }

  virtual bool get_work(uint64_t id, Work *&work, WorkerStat &stat)
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


      auto start = std::chrono::high_resolution_clock::now();      
      if (cfg.sleep_timeout_ms == 0) {
        condition_work_.wait(guard);
        cv_status = std::cv_status::no_timeout;
      } else {
        cv_status = condition_work_.wait_for(guard, std::chrono::milliseconds(cfg.sleep_timeout_ms));
      }
      auto end = std::chrono::high_resolution_clock::now();
      stat.sleep_time += std::chrono::nanoseconds(end - start).count();
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

    for (size_t i = 0; i < worker_cfg_.size(); i++) {
      worker_cfg_[i].queue_retry_cnt = 100;
      worker_cfg_[i].sleep_timeout_ms = 97 + i;
    }


    // and do nothing
  }
};

class AdvCleverWorkerFarm : public AdvWorkerFarm
{
private:
  // number of worker, that should sleep and not be terminated
  size_t idle_worker_cnt_;

  std::atomic<bool> stop_supervisor_;
  std::atomic<uint64_t> lowest_stop_id_;

  std::list<std::thread> threads_;
  std::thread supervisor_;

  std::mutex supervisor_mutex_;
  std::condition_variable condition_notify_supervisor_;
public:
  AdvCleverWorkerFarm(size_t queue_size, size_t max_worker_cnt, size_t idle_worker_cnt) :
    AdvWorkerFarm(queue_size, max_worker_cnt),
    idle_worker_cnt_(idle_worker_cnt),
    stop_supervisor_(false),
    lowest_stop_id_(0),
    supervisor_([this](){ this->run_supervisor(); })
  {}

  ~AdvCleverWorkerFarm() {}

  virtual void run(WorkerStat &stat)
  {
    throw 0;
  }

  virtual void stopWhenDone()
  {
    std::lock_guard<std::mutex> guard(supervisor_mutex_);
    stop_supervisor_.store(true);
    condition_notify_supervisor_.notify_all();
    AdvWorkerFarm::stopWhenDone();
  }

  virtual void stop()
  {
    std::unique_lock<std::mutex> guard(supervisor_mutex_);
    stop_supervisor_.store(true);
    condition_notify_supervisor_.notify_all();
    guard.unlock();

    supervisor_.join();

    AdvWorkerFarm::stop();

    for (auto &thrd : threads_) {
      thrd.join();
    }
  }

  void run_supervisor()
  {
    pthread_setname_np(pthread_self(), "server-sv");
    // Initialise everything
    wakeup_queue_length = 5;
    wakeup_time_ns_ = 300000;
    definitive_wakeup_time_ns_ = 61803300;

    for (size_t i = 0; i < worker_cfg_.size(); i++) {
      worker_cfg_[i].queue_retry_cnt = 100;
      worker_cfg_[i].sleep_timeout_ms = 97 + i;
    }

    while (worker_cnt_ < idle_worker_cnt_ && worker_cnt_ < max_worker_cnt_) {
      start_new_thread();
    }


    /*
     *  Compute jobs done per sv tick. Test if the current jobs/s is enough
     *  to work on all jobs in at most 4 ticks. If not, start another thread.
     *
     *
     */


    int jobs_stalling_tick = 0, queue_length_dt_tick = 0;
    uint64_t last_jobs_done = 0, jobs_done;
    uint64_t /*last_jobs_submitted = 0,*/ jobs_submitted;
    uint64_t last_queue_length = 0, queue_length, queue_length_dt;

    while (!stop_supervisor_) {

      jobs_done = jobs_done_.load(std::memory_order_acquire);
      jobs_submitted = jobs_submitted_.load(std::memory_order_relaxed);


      if (jobs_done == last_jobs_done && (jobs_done < jobs_submitted)) {
        jobs_stalling_tick++;
      } else {
        jobs_stalling_tick = jobs_stalling_tick == 0 ? 0 : jobs_stalling_tick - 1;
      }

      queue_length = jobs_submitted - jobs_done;

      queue_length_dt = (queue_length - last_queue_length) / 10;


      if (queue_length_dt > 100) {
        queue_length_dt_tick++;
      } else {
        queue_length_dt_tick--;
        if (queue_length_dt_tick < 0) {
          queue_length_dt_tick = 0;
        }
      }




      bool do_start_new_thread = (jobs_stalling_tick > 5) || ((queue_length >= 50 ) && (1.5 * last_queue_length >= queue_length));

      bool do_stop_one_thread = (queue_length == 0);

      last_jobs_done = jobs_done;
      //last_jobs_submitted = jobs_submitted;
      last_queue_length = queue_length;


      if (do_start_new_thread && worker_cnt_ < max_worker_cnt_) {

        jobs_stalling_tick = 0;
        queue_length_dt_tick = 0;

        start_new_thread();

      } else if (do_stop_one_thread && worker_cnt_ > idle_worker_cnt_) {
        stop_one_thread();
      }

      if (stop_supervisor_) {
        break ;
      }

      std::unique_lock<std::mutex> guard(supervisor_mutex_);
      auto status = condition_notify_supervisor_.wait_for(guard, std::chrono::milliseconds(10));

      if (status != std::cv_status::timeout) {
        break ;
      }
    }
  }

protected:
  void start_new_thread()
  {
    std::cout<<"SV: Starting new thread"<<std::endl;

    std::unique_lock<std::mutex> guard(supervisor_mutex_);
    lowest_stop_id_ += 1;

    threads_.emplace_back([this]() {
      this->run_internal();
    });

    // wait for the new thread to be fully functional
    condition_notify_supervisor_.wait(guard);
    std::cout<<"SV: Thread active"<<std::endl;
  }

  void stop_one_thread()
  {
    std::cout<<"SV: stoping thread"<<std::endl;
    {
      std::unique_lock<std::mutex> guard(mutex_);
      lowest_stop_id_ -= 1;
      condition_work_.notify_all();
    }
    threads_.back().join();
    threads_.pop_back();
    std::cout<<"SV: thread stoped"<<std::endl;
  }

  bool worker_init_internal(uint64_t &id)
  {
    std::lock_guard<std::mutex> guard(supervisor_mutex_);
    pthread_setname_np(pthread_self(), "server-worker");

    bool success = worker_init (id);

    std::cout<<"New thread "<<id<<std::endl;

    condition_notify_supervisor_.notify_one();
    return success;
  }

  virtual bool get_work(uint64_t id, Work *&work)
  {
    while (!should_stop_.load(std::memory_order_relaxed) && id < lowest_stop_id_)
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

      if (stop_when_done_ || should_stop_ || id >= lowest_stop_id_) {
        break ;
      }

      if (cfg.sleep_timeout_ms == 0) {
        condition_work_.wait(guard);
      } else {
        condition_work_.wait_for(guard, std::chrono::milliseconds(cfg.sleep_timeout_ms));
      }
    }

    return false;
  }

  void run_internal()
  {
    uint64_t id;

    if(!worker_init_internal(id)) {
      return ;
    }

    Work *work;

    while (get_work(id, work)) {
      work->doit();
      jobs_done_.fetch_add(1, std::memory_order_release);

      delete work;
    }

    worker_cnt_--;
  }
};

#endif
