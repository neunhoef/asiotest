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
#include <sstream>
#include <iomanip>

#include <boost/lockfree/queue.hpp>

#include <unistd.h>

#include "richard_worker_farm.h"
#include "futex_worker_farm.h"
#include "lockfree_richard_worker_farm.h"
#include "std_worker_farm.hpp"

std::string prettyTime(uint64_t nanoseconds) {  
  double val;
  const char* unit;
  if (nanoseconds < 10000) {
    val = nanoseconds;
    unit = " ns";
  } else if (nanoseconds < 10000000) {
    val = (nanoseconds / 10) / 100.0;
    unit = " us";
  } else if (nanoseconds < 10000000000) {
    val = (nanoseconds / 10000) / 100.0;
    unit = " ms";
  } else {
    val = (nanoseconds / 10000000) / 100.0;
    unit = " s";
  }
  std::ostringstream oss;
  oss << std::setw(7) << std::setprecision(2) << std::fixed << val << " " << unit;
  return oss.str();
}

struct IOStats
{
  cacheline_pad_t pad_0;
  uint64_t num_submits;
  uint64_t submit_time;
  uint64_t run_time;
  cacheline_pad_t pad_1;

  std::vector<uint32_t> submit_times;
};

#define SUBMITS_PER_THREAD 500000

int main(int argc, char* argv[])
{
  try {

  	// Worker threads do work that should take ~20us. The `iothreads` insert work into the queue every
  	// x ms. This should test the performance of the `backend`.

    if (argc != 5) {
      std::cerr << "Usage: worker_test <nriothreads> <nrthreads> <workerdelay> <impl>\n"
        << "Implementations: 1 - Richard, 2 - Futex (Manuel), 3 - Lockfree Richard, 4 - Std Lockfree\n";
      return 1;
    }

    WorkerFarm *workerFarm;

    int nrIOThreads = std::atoi(argv[1]);
    int nrThreads 	= std::atoi(argv[2]);
    uint64_t globalDelay 	= std::atoi(argv[3]);

    switch (std::atoi(argv[4]))
    {
      case 1:
        std::cout<<"Testing using Richards impl. (with locks)"<<std::endl;
        workerFarm = new RichardWorkerFarm(10000000);
        break ;
      case 2:
        std::cout<<"Testing using Futex impl."<<std::endl;
        workerFarm = new FutexWorkerFarm(10000000);
        break ;
      case 3:
        std::cout<<"Testing using Richards impl. (lock free)"<<std::endl;
        workerFarm = new LockfreeRichardWorkerFarm(10000000);
        break ;
      case 4:
      default:
        std::cout<<"Testing std. mutex worker farm"<<std::endl;
        workerFarm = new StdWorkerFarm(10000000);
        break ;
    }

    std::vector<std::thread> threads;

    std::vector<WorkerStat> stats(nrThreads);
    std::vector<IOStats> iostats(nrIOThreads);

    for (int i = 0; i < nrThreads; ++i) {
      threads.emplace_back([i, &stats, workerFarm]() { workerFarm->run(stats[i]); });
    }

    for (int i = 0; i < nrIOThreads; ++i) {
        threads.emplace_back([i, &iostats, workerFarm, globalDelay]() {

        IOStats &stat = iostats[i];
        stat.submit_times.reserve(SUBMITS_PER_THREAD);

        auto run_start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < SUBMITS_PER_THREAD / 10; i++)
        {
          uint64_t submit_time_acc = 0;

          for (int j = 0; j < 10; j++) {
            CountWork* work = new CountWork([](){}, globalDelay);
            auto submit_start = std::chrono::high_resolution_clock::now();
            workerFarm->submit(work);
            auto submit_stop = std::chrono::high_resolution_clock::now();

            stat.num_submits++;
            uint64_t submit_time = std::chrono::nanoseconds(submit_stop - submit_start).count();
            stat.submit_time += submit_time;
            submit_time_acc += submit_time;

            stat.submit_times.push_back(submit_time);
          }

          int64_t sleep_time = 20 * 10 - submit_time_acc / 1000;
          if (sleep_time > 0) {
            usleep(0);
          }
        }

        auto run_end = std::chrono::high_resolution_clock::now();

        stat.run_time = std::chrono::nanoseconds(run_end - run_start).count();
        std::sort(stat.submit_times.begin(), stat.submit_times.end());
      });
    }

    // wait for the IO threads to finish their job
    for (size_t i = nrThreads; i < threads.size(); ++i) {
      threads[i].join();
    }

    std::cout<<"IO Threads done. Wait for farm."<<std::endl;
    workerFarm->stopWhenDone();

    // now wait for the worker threads to end
    for (int i = 0; i < nrThreads; ++i) {
      threads[i].join();
    }

    double totalTime = 0;
    double totalWork = 0;

    // now aggregate the statistics
    std::cout << "== Worker Threads ==" << std::endl;
    for (int i = 0; i < nrThreads; i++) {
      auto& stat = stats[i];
    	std::cout << i <<
        "\tjobs:   "          << std::setw(7) << stat.num_work <<
        "\twork_time:  "      << prettyTime(stat.work_time) <<
        "\tavg. work_time:  " << prettyTime(stat.work_time / stat.num_work) <<
        "\tavg. w/s: "        << (int) (1000000000.0 * stat.num_work / stat.work_time) <<
        "\n\tsleeps: "        << std::setw(7) << stat.num_sleeps <<
        "\tsleep_time: "      << prettyTime(stat.sleep_time) <<
        "\tavg. sleep_time: " << prettyTime(stat.sleep_time / stat.num_sleeps) << std::endl;
      totalTime += stat.work_time;
      totalWork += stat.num_work;
    }

    std::cout << "Avg. Work: " << totalWork / nrThreads <<
      "\tWork/Sec: " << 1000000000 * totalWork / (totalTime / nrThreads) << std::endl;

    std::cout << "== IO Threads ==" << std::endl;
    for (int i = 0; i < nrIOThreads; i++) {
      std::cout << i <<
        "\tsubmits: " << iostats[i].num_submits <<
        "\tavg. submit_time: " << prettyTime(iostats[i].submit_time / iostats[i].num_submits) <<
        "\tavg. time/submit:" << prettyTime(iostats[i].run_time / iostats[i].num_submits) <<
        "\n\tsubmit median: " << prettyTime(iostats[i].submit_times[SUBMITS_PER_THREAD/2]) <<
        "\tsubmit 90%: " << prettyTime(iostats[i].submit_times[(SUBMITS_PER_THREAD*90)/100]) <<
        "\tsubmit 99%: " << prettyTime(iostats[i].submit_times[(SUBMITS_PER_THREAD*99)/100]) <<
        "\tsubmit 99.9%: " << prettyTime(iostats[i].submit_times[(SUBMITS_PER_THREAD*999)/1000]) << std::endl;
    }

    delete workerFarm;

  }
  catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
