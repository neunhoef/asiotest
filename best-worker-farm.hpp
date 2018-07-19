#ifndef BEST_WORKER_FARM_H
#define BEST_WORKER_FARM_H
#include <list>
#include <boost/lockfree/queue.hpp>
#include "worker_farm.h"



class workerfarm {
public:
  virtual ~workerfarm();
  virtual bool submit(Work *work) = 0;
  virtual void stop() = 0;
  virtual void stop_when_done() = 0;
};


class supervised_workerfarm : public workerfarm {

  struct worker {
    std::thread thread_;
  };

protected:
  std::atomic<bool> supervisor_running_;
  std::mutex supervisor_mutex_;
  std::condition_variable supervisor_cv_;
  std::thread supervisor_;

protected:
  std::list<worker> worker_;

public:

  static constexpr uint64_t supervisor_sleep_time_ms = 10;

  supervised_workerfarm(size_t queue_size, size_t idel_worker_cnt, size_t max_worker_cnt) :
    supervisor_running_(true),

    supervisor_([this](){ this->run_supervisor(); })
  {}

  bool submit(Work *work)
  {
    return false;
  }

  virtual void stop() {

  }

  virtual void stop_when_done() {

  }

protected:
  virtual void init_supervisor() {
    // update thread name
    set_thread_name ("supervisor");
  };

  void run_supervisor() {
    init_supervisor();

    while (supervisor_running_.load(std::memory_order_relaxed)) {
      exec_superviser();

      std::lock_guard<std::mutex> guard(supervisor_mutex_);
      auto status = std::chrono::milliseconds(supervisor_sleep_time_ms);
    }
  };

  virtual void stop_supervisor() {
    if (supervisor_running_.load(std::memory_order_relaxed)) {
      std::lock_guard<std::mutex> guard(supervisor_mutex_);
      supervisor_running_.store(false);
      supervisor_cv_.notify_all();
    }

    supervisor_.join();
  };

  virtual void run_worker() {

  };




  virtual void exec_superviser() = 0;
  virtual void exec_work() = 0;


  virtual bool get_work(uint64_t id, Work *&work, WorkerStat &stat)
  {
    return false;
  }
};



#endif
