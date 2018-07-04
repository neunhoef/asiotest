#ifndef WORKER_FARM_H
#define WORKER_FARM_H

static size_t const     cacheline_size = 64;
typedef char            cacheline_pad_t [cacheline_size];

struct WorkerStat {
  cacheline_pad_t pad_0;

  uint64_t num_sleeps;
  
  uint64_t work_time;
  uint64_t num_work;

  cacheline_pad_t pad_1;

  WorkerStat() : num_sleeps(0), work_time(0), num_work(0) {}
};

struct Work {
  virtual ~Work() {};
  virtual void doit() = 0;
};

class WorkerFarm
{
public:
	virtual ~WorkerFarm() {};

	virtual bool submit(Work *work) = 0;

	virtual void stopWhenDone() = 0;
	virtual void stop() = 0;
	virtual void run(WorkerStat &stat) = 0;
};

class CountWork : public Work {

  uint64_t dummy_, delay_;
  std::function<void()> completion_;

 public:
  CountWork(std::function<void()>&& c, uint64_t delay) : dummy_(0), delay_(delay), completion_(c){
  }

  void doit() override final {
    delayRunner(delay_);
    completion_();
  }

 private:
  uint64_t delayRunner(uint64_t delay) {
    for (uint64_t i = 0; i < delay; ++i) {
      dummy_ += i * i;
    }
    return dummy_;
  }

};


#endif