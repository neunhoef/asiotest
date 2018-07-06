#ifndef STD_WORKER_FARM
#define STD_WORKER_FARM

#include <boost/lockfree/queue.hpp>

#include "worker_farm.h"


#define STDWF_CNT_SLEEPING(x)   ((x) & 0xFFFFFFFF)
#define STDWF_CNT_QUEUE_LEN(x)  ((x) >> 32)

#define STDWD_CNT_ONE_WORK      (((uint64_t)1) << 32)
#define STDWD_CNT_ONE_SLEEPER   (1)

class StdWorkerFarm : public WorkerFarm
{
    boost::lockfree::queue<Work*> _queue;

    // _counter stores number of sleeping threads in upper 32 bits
    // and number of current work queue length in lower 32 bits
    std::atomic<uint64_t> _counter;
    std::atomic<uint32_t> _numWorker;

    std::atomic<bool> _shouldStop;
    std::atomic<bool> _stopWhenDone;

    std::mutex _mutex;
    std::condition_variable _condition;

public:
    StdWorkerFarm(size_t maxQueueLength) : _queue(maxQueueLength) {}

    virtual bool submit(Work *work) {

        if (!_queue.push(work))
            return false;

        uint64_t counter = _counter.fetch_add
            (STDWD_CNT_ONE_WORK, std::memory_order_seq_cst);

        if (STDWF_CNT_SLEEPING(counter) > 0) {
            std::lock_guard<std::mutex> guard(_mutex);
            _condition.notify_one();
        }

        return true;
    }

    virtual void stopWhenDone() {
        _stopWhenDone.store(true, std::memory_order_relaxed);
        _condition.notify_all();
    }

    virtual void stop() {
        _shouldStop.store(true, std::memory_order_relaxed);
        _condition.notify_all();
    }

    virtual void run(WorkerStat &stat) {

        _numWorker++;

        while (true) {

            Work *work = getWork(stat);

            if (work == nullptr) {
                break ;
            }

            stat.num_work++;
            auto start = std::chrono::high_resolution_clock::now();
            work->doit();
            auto end = std::chrono::high_resolution_clock::now();
            stat.work_time += std::chrono::nanoseconds(end - start).count();
            delete work;
        }

        _numWorker--;
    }

private:
    Work *getWork(WorkerStat &stat) {

        Work *work;

        while (!_shouldStop.load(std::memory_order_relaxed)) {

            const int maxRetries = 3;

            for (int i = 0; i < maxRetries; i++) {
                if (_queue.pop(work)) {
                    // Why is here memory order relaxed?
                    /*uint64_t counter = */_counter.fetch_sub
                        (STDWD_CNT_ONE_WORK, std::memory_order_relaxed);

                    // this is copied from futex implementation
                    /*if (STDWF_CNT_QUEUE_LEN(counter) >= 10) {
                        _condition.notify_one();
                    }*/

                    return work;
                }

                if (i < maxRetries - 1) {
                    std::this_thread::yield();
                }
            }


            // no work, go to sleep
            if (_stopWhenDone.load(std::memory_order_relaxed)) {
                break ;
            }

            std::unique_lock<std::mutex> guard(_mutex);
            uint64_t counter = _counter.fetch_add
                (STDWD_CNT_ONE_SLEEPER, std::memory_order_seq_cst);

            if (STDWF_CNT_QUEUE_LEN(counter) == 0) {
                _condition.wait(guard);
            }

            _counter.fetch_add (STDWD_CNT_ONE_SLEEPER, std::memory_order_relaxed);
        }

        return nullptr;
    }
};

#endif
