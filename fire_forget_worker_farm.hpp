#ifndef FIRE_AND_FORGET_WORKER_FARM
#define FIRE_AND_FORGET_WORKER_FARM

#include <boost/lockfree/queue.hpp>

#include "worker_farm.h"

class FireForgetWorkerFarm : public WorkerFarm
{
    boost::lockfree::queue<Work*> _queue;

    std::atomic<uint32_t> _queue_length;
    std::atomic<uint32_t> _threads_running;

    std::atomic<bool> _sleepy;
    std::mutex _mutex;
    std::condition_variable _condition;


    std::atomic<bool> _shouldStop;
    std::atomic<bool> _stopWhenDone;

public:
    FireForgetWorkerFarm(size_t queueSize) : _queue(queueSize), _queue_length(0), _threads_running(0),_sleepy(ATOMIC_FLAG_INIT) {}

    virtual bool submit(Work *work)
    {
        if (!_queue.push(work))
            return false;

        if (_sleepy.load()) {
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

    virtual void run(WorkerStat &stat)
    {
        uint32_t thread_id = _threads_running++;

        while (thread_id == 0) {

            Work *work = getWork(stat, thread_id);

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

        _threads_running--;
    }

private:
    Work *getWork(WorkerStat &stat, uint32_t thread_id)
    {
        Work *work;

        while (!_shouldStop.load(std::memory_order_relaxed)) {

            if (_queue.pop(work)) {
                return work;
            }

            if (_stopWhenDone.load(std::memory_order_relaxed)) {
                break ;
            }

            _sleepy.store(true);

            {
                stat.num_sleeps++;
                auto start = std::chrono::high_resolution_clock::now();
                std::unique_lock<std::mutex> guard(_mutex);
                _condition.wait_for(guard, std::chrono::milliseconds(10));
                auto end = std::chrono::high_resolution_clock::now();
                stat.sleep_time += std::chrono::nanoseconds(end - start).count();
            }

            _sleepy.store(false);
        }

        return nullptr;
    }
};

#endif

