#ifndef STD_WORKER_FARM
#define STD_WORKER_FARM

#include <boost/lockfree/queue.hpp>

#include "worker_farm.h"


#define STDWF_CNT_SLEEPING(x)   ((x) & 0xFFFFFFFF)
#define STDWF_CNT_QUEUE_LEN(x)  ((int32_t) ((x) >> 32))

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
    int64_t _queueMaxLength;

    StdWorkerFarm(size_t maxQueueLength) : _queue(maxQueueLength), _counter(0), _numWorker(0), _queueMaxLength(0) {}

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

    virtual bool submit(Work *work) {

        // (1)
        uint64_t counter = _counter.fetch_add
            (STDWD_CNT_ONE_WORK, std::memory_order_seq_cst);

        if (!_queue.push(work))
            return false;

        if (STDWF_CNT_SLEEPING(counter) > 0) {
            std::lock_guard<std::mutex> guard(_mutex);
            _condition.notify_one();
        }

        return true;
    }
private:
    Work *getWork(WorkerStat &stat) {

        Work *work;

        while (!_shouldStop.load(std::memory_order_relaxed)) {

            const int maxRetries = 3000;

            for (int i = 0; i < maxRetries; i++) {
                if (_queue.pop(work)) {

                    uint64_t counter = _counter.fetch_sub
                        (STDWD_CNT_ONE_WORK, std::memory_order_relaxed);

                    if (stat.num_work & 0x3FF) {
                        if (STDWF_CNT_QUEUE_LEN(counter) > _queueMaxLength) {
                            _queueMaxLength = STDWF_CNT_QUEUE_LEN(counter);
                        }
                    }

                    return work;
                }

                if (i < maxRetries - 1) {
                    cpu_relax(); //std::this_thread::yield();
                }

                uint64_t counter = _counter.load(std::memory_order_relaxed);

                if (STDWF_CNT_QUEUE_LEN(counter) > 0) {
                    i--;
                }
            }


            // no work, go to sleep
            if (_stopWhenDone.load(std::memory_order_relaxed)) {
                break ;
            }

            std::unique_lock<std::mutex> guard(_mutex);

            // (2)
            uint64_t counter = _counter.fetch_add
                (STDWD_CNT_ONE_SLEEPER, std::memory_order_seq_cst);

            if (STDWF_CNT_QUEUE_LEN(counter) == 0) {
                stat.num_sleeps++;

                if (STDWF_CNT_SLEEPING(counter) >= _numWorker.load(std::memory_order_relaxed)) {
                    _condition.wait_for(guard, std::chrono::milliseconds(100));
                } else {
                    _condition.wait(guard);
                }

            }

            _counter.fetch_sub (STDWD_CNT_ONE_SLEEPER, std::memory_order_relaxed);
        }

        return nullptr;
    }
};

#endif
