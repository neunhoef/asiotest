#ifndef WORKER_FARM_H
#define WORKER_FARM_H

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
	virtual void run() = 0;
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