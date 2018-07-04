#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <deque>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <algorithm>
#include <fstream>

#include <unistd.h>

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

// Derive from this class to submit work to the WorkerFarm:
struct Work {
  virtual void doit() {
  }
};

static size_t const     cacheline_size = 64;
typedef char            cacheline_pad_t [cacheline_size];

struct WorkerStat {
  cacheline_pad_t pad_0;
  uint64_t num_sleeps;
  uint64_t work_time;
  uint64_t num_work;
  uint64_t run_time;
  uint64_t wait_time;

  cacheline_pad_t pad_1;

  WorkerStat() : num_sleeps(0), work_time(0), num_work(0) {}
};

class WorkerFarm {

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
  WorkerFarm(size_t maxQueueLen) 
    : nrThreadsInRun_(0), nrThreadsAwake_(0), num_sleeps(0), shouldStop_(false), stopIfFinish_(false), tick(0), maxQueueLen_(maxQueueLen) {
  }

  ~WorkerFarm() {
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
    auto run_start = std::chrono::high_resolution_clock::now();
    while (true) {
      auto start = std::chrono::high_resolution_clock::now();
      {
	      std::unique_ptr<Work> work = getWork(stat);
	      if (work == nullptr || shouldStop_) {
	        break ;
	      }
	      stat.num_work++;
	      
	      work->doit();
      }
      auto end = std::chrono::high_resolution_clock::now();
      stat.work_time += std::chrono::nanoseconds(end - start).count();
    }

    auto run_end = std::chrono::high_resolution_clock::now();

    stat.run_time = std::chrono::nanoseconds(run_end - run_start).count();
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
      auto wait_start = std::chrono::high_resolution_clock::now();
      sleeper->cond_.wait(guard);
      auto wait_end = std::chrono::high_resolution_clock::now();
      stat.wait_time += std::chrono::nanoseconds(wait_end - wait_start).count();
      nrThreadsAwake_++;
    }


    return nullptr;
  }
 
};

WorkerFarm workerFarm(10000000);

uint64_t globalDelay = 1;

class CountWork : public Work {

  uint64_t dummy_;
  std::function<void()> completion_;

 public:
  CountWork(std::function<void()>&& c) : dummy_(0), completion_(c) {
  }

  void doit() override final {
    delayRunner(globalDelay);
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

struct IOStats
{
  cacheline_pad_t pad_0;
  uint64_t num_submits;
  uint64_t submit_time;
  uint64_t run_time;
  cacheline_pad_t pad_1;
};

int main(int argc, char* argv[])
{
  try {

  	// Worker threads do work that should take ~20us. The `iothreads` insert work into the queue every
  	// x ms. This should test the performance of the `backend`.

    if (argc != 5) {
      std::cerr << "Usage: worker_test <nriothreads> <nrthreads> <workerdelay> <queuestate-file>\n";
      return 1;
    }

    int nrIOThreads = std::atoi(argv[1]);
    int nrThreads 	= std::atoi(argv[2]);
    globalDelay 	= std::atoi(argv[3]);

    std::vector<std::thread> threads;

    std::vector<WorkerStat> stats(nrThreads);
    std::vector<IOStats> iostats(nrIOThreads);
    
    for (int i = 0; i < nrThreads; ++i) {
      threads.emplace_back([i, &stats]() { workerFarm.run(stats[i]); });
    }


    for (int i = 0; i < nrIOThreads; ++i) {
      threads.emplace_back([i, &iostats]() {

      	IOStats &stat = iostats[i];

      	auto run_start = std::chrono::high_resolution_clock::now();

      	for (int i = 0; i < 5000; i++)
      	{
      		uint64_t submit_time_acc = 0;

      		for (int j = 0; j < 100; j++) {
	      		auto submit_start = std::chrono::high_resolution_clock::now();
	      		CountWork* work = new CountWork([](){});
	            workerFarm.submit(work);
	            auto submit_stop = std::chrono::high_resolution_clock::now();

	            stat.num_submits++;
	            uint64_t submit_time = std::chrono::nanoseconds(submit_stop - submit_start).count();
	            stat.submit_time += submit_time;	
	            submit_time_acc += submit_time;
      		} 


            int64_t sleep_time = 20 * 100 - submit_time_acc / 1000;
            if (sleep_time > 0) {
            	usleep(0);
            }
       
      	}

      	auto run_end = std::chrono::high_resolution_clock::now();

      	stat.run_time = std::chrono::nanoseconds(run_end - run_start).count();
      });
    }

    std::fstream output(argv[4], std::fstream::out | std::fstream::trunc);
    bool stopStatReader = false;


    std::thread statReader([&output, &stopStatReader] () {
    	uint64_t t = 0;

    	while (!stopStatReader)
    	{
    		uint64_t queue_size;

	    	// get the locks
	    	workerFarm.FUTEX_LOCK;
	    	workerFarm.mutex_.lock();
	    	queue_size = workerFarm.workQueue_.size();
	    	workerFarm.mutex_.unlock();
	    	workerFarm.FUTEX_UNLOCK;


	    	output << t++ << " " <<  queue_size << std::endl;

	    	usleep(50000);
    	}

    	std::cout<<"Reader stopped."<<std::endl;
    });

    // wait for the IO threads to finish their job
    for (size_t i = nrThreads; i < threads.size(); ++i) {
      threads[i].join();
    }

    std::cout<<"IO Threads done. Wait for farm."<<std::endl;
    workerFarm.stopWhenDone();

    // now wait for the worker threads to end
    for (int i = 0; i < nrThreads; ++i) {
      threads[i].join();
    }

    stopStatReader = true;
    statReader.join();

    // now aggregate the statistics
    for (int i = 0; i < nrThreads; i++) {
    	std::cout<< i << " sleeps: " << stats[i].num_sleeps << " work_num: " << stats[i].num_work << " work_time: " << stats[i].work_time << "ns avg. work_time: " <<
    	 	stats[i].work_time / (1000.0 * stats[i].num_work) << "ns run_time: " << stats[i].run_time <<
    	 	"ns wait_time: " << stats[i].wait_time << "ns" << std::endl;
    }

    for (int i = 0; i < nrIOThreads; i++) {
    	std::cout<< i << " num_submits: " << iostats[i].num_submits << " submit_time: " << iostats[i].submit_time << "ns avg. submit_time: " <<
    		 iostats[i].submit_time / iostats[i].num_submits << "ns run_time: " << iostats[i].run_time << "ns avg. time/submit:" << 
    		 iostats[i].run_time / iostats[i].num_submits << "ns" << std::endl;
    }

    /*std::cout<<" IO="<<nrIOThreads<<" W="<<nrThreads<<" sleeps="<<aggre.num_sleeps<<" spin_count="<<aggre.spin_count<<" spin_tries="<<aggre.spin_tries
      <<" s-avg="<<(aggre.spin_count/aggre.spin_tries) <<std::endl;*/

  }
  catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}