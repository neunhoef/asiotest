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

#include <boost/lockfree/queue.hpp>

#include <unistd.h>

#include "richard_worker_farm.h"
#include "futex_worker_farm.h"
#include "lockfree_richard_worker_farm.h"


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

    if (argc != 6) {
      std::cerr << "Usage: worker_test <nriothreads> <nrthreads> <workerdelay> <queuestate-file> <impl>\n"
        << "Implementations: 1 - Richard, 2 - Futex (Manuel), 3 - Lockfree Richard\n";
      return 1;
    }

    WorkerFarm *workerFarm;

    int nrIOThreads = std::atoi(argv[1]);
    int nrThreads 	= std::atoi(argv[2]);
    uint64_t globalDelay 	= std::atoi(argv[3]);

    switch (std::atoi(argv[5]))
    {
      case 1:
        std::cout<<"Testing using Richards impl. (with locks)"<<std::endl;
        workerFarm = new RichardWorkerFarm(100000000);
        break ;
      case 2:
        std::cout<<"Testing using Futex impl."<<std::endl;
        workerFarm = new FutexWorkerFarm(100000000);
        break ;
      case 3:
      default:
        std::cout<<"Testing using Richards impl. (lock free)"<<std::endl;
        workerFarm = new LockfreeRichardWorkerFarm(100000000);
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

      	auto run_start = std::chrono::high_resolution_clock::now();

      	for (int i = 0; i < 50000; i++)
      	{
      		uint64_t submit_time_acc = 0;

      		for (int j = 0; j < 10; j++) {
	      		auto submit_start = std::chrono::high_resolution_clock::now();
	      		CountWork* work = new CountWork([](){}, globalDelay);
	            workerFarm->submit(work);
	            auto submit_stop = std::chrono::high_resolution_clock::now();

	            stat.num_submits++;
	            uint64_t submit_time = std::chrono::nanoseconds(submit_stop - submit_start).count();
	            stat.submit_time += submit_time;	
	            submit_time_acc += submit_time;
      		} 


            int64_t sleep_time = 20 * 10 - submit_time_acc / 1000;
            if (sleep_time > 0) {
            	usleep(0);
            }
       
      	}

      	auto run_end = std::chrono::high_resolution_clock::now();

      	stat.run_time = std::chrono::nanoseconds(run_end - run_start).count();
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

    //double totalTime = 0;
    //double totalWork = 0;

    // now aggregate the statistics
    for (int i = 0; i < nrThreads; i++) {
    	std::cout<< i << " sleeps: " << stats[i].num_sleeps << " work_num: " << stats[i].num_work << " work_time: " << stats[i].work_time << "ns avg. work_time: " <<
    	 	stats[i].work_time / (1000.0 * stats[i].num_work) << "ns avg. w/s: " 
        << (int) (1000000000.0 * stats[i].num_work / stats[i].work_time) <<  std::endl;
    	//totalWork += stats[i].num_work;
    	//totalTime += stats[i].run_time;
    }

    //std::cout<<"Avg. Work: "<<  totalWork / nrThreads << " Work/Sec: "<< 1000000000 * totalWork / ( totalTime / nrThreads) <<std::endl;


    for (int i = 0; i < nrIOThreads; i++) {
    	std::cout<< i << " num_submits: " << iostats[i].num_submits << " submit_time: " << iostats[i].submit_time << "ns avg. submit_time: " <<
    		 iostats[i].submit_time / iostats[i].num_submits << "ns run_time: " << iostats[i].run_time << "ns avg. time/submit:" << 
    		 iostats[i].run_time / iostats[i].num_submits << "ns" << std::endl;
    }

    /*std::cout<<" IO="<<nrIOThreads<<" W="<<nrThreads<<" sleeps="<<aggre.num_sleeps<<" spin_count="<<aggre.spin_count<<" spin_tries="<<aggre.spin_tries
      <<" s-avg="<<(aggre.spin_count/aggre.spin_tries) <<std::endl;*/

    delete workerFarm;

  }
  catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}