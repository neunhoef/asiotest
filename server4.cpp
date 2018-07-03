#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <boost/lockfree/queue.hpp>
#include "asio.hpp"

using asio::ip::tcp;

class Futex {
public:
  Futex() : _val() {}
  Futex(int val) : _val(val) {}

  std::atomic<int>& value() { return _val; }

  void wait(int expectedValue);

  void notifyOne();

  void notifyAll();
private:
  std::atomic<int> _val;
};

#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>

namespace {
  static int futex(std::atomic<int>* uaddr, int futex_op, int val,
    const struct timespec *timeout, int *uaddr2, int val3)
  {
    return syscall(SYS_futex, reinterpret_cast<int*>(uaddr), futex_op, val, timeout, uaddr, val3);
  }
}

void Futex::wait(int expectedValue) {
  futex(&_val, FUTEX_WAIT_PRIVATE, expectedValue, nullptr, nullptr, 0);
}

void Futex::notifyOne() {
  futex(&_val, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void Futex::notifyAll() {
  futex(&_val, FUTEX_WAKE_PRIVATE, std::numeric_limits<int>::max(), nullptr, nullptr, 0);
}
#endif

// Derive from this class to submit work to the WorkerFarm:
struct Work {
  virtual ~Work() {}
  virtual void doit() {}
};

class WorkerFarm {
  boost::lockfree::queue<Work*>    workQueue_;
  Futex size_;

  std::atomic<bool> shouldStop_;
  std::atomic<unsigned> waiting_;
  std::atomic<unsigned> nrThreadsInRun_;

  int maxQueueLen_; // has to be int because futexes only support 32-bit int

 public:
  WorkerFarm(int maxQueueLen) 
    : workQueue_(maxQueueLen), size_(0), shouldStop_(false), waiting_(0), nrThreadsInRun_(0), maxQueueLen_(maxQueueLen) {
  }

  ~WorkerFarm() {
    while (nrThreadsInRun_ > 0) {
      usleep(100);
    }
  }

  bool submit(Work* work) {
    // Returns true if successfully submitted and false if rejected  
    if (size_.value().load(std::memory_order_relaxed) + 1 >= maxQueueLen_)
      return false;
      
    if (!workQueue_.push(work))
      return false;
      
    auto pendingJobs = size_.value().fetch_add(1, std::memory_order_seq_cst);
    
    // (1) - this seq-cst load ensures a total order with the seq-cst fetch-add (2) 
    if (waiting_.load(std::memory_order_seq_cst) == nrThreadsInRun_.load(std::memory_order_relaxed) || pendingJobs >= 20)
      size_.notifyOne();
    return true;
  }

  // Arbitrarily many threads can call this to join the farm:
  void run() {
    nrThreadsInRun_++;
    while (true) {
      Work* work = getWork();
      if (work == nullptr || shouldStop_.load(std::memory_order_relaxed)) {
        delete work;
        break;
      }
      work->doit();
      delete work;
    }
    nrThreadsInRun_--;
  }

  // Call this from any thread to stop the work farm, work items will be
  // completed but no new work is begun. This function returns immediately,
  // the destructor waits until all threads running in the run method have
  // left it.
  void stop() {
    shouldStop_.store(true, std::memory_order_relaxed);
    size_.notifyAll();
  }

 private:
  Work* getWork() {
    while (!shouldStop_.load(std::memory_order_relaxed))
    {
      const int max_tries = 3; // try a few times to fetch a job before going to sleep
      for (int i = 0; i < max_tries; ++i) {
        Work* work;
        if (workQueue_.pop(work)) {
          if (size_.value().fetch_add(-1, std::memory_order_relaxed) >= 10)
            size_.notifyOne();
          return work;
        }
        
        if (i < max_tries - 1)
          std::this_thread::yield();
      }
      
      // (2) - this seq-cst fetch-add ensures a total order with the seq-cst load (1)
      waiting_.fetch_add(1, std::memory_order_seq_cst);
      size_.wait(0);
      waiting_.fetch_add(-1, std::memory_order_relaxed);
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


class Connection : public std::enable_shared_from_this<Connection> {
public:
  Connection(tcp::socket socket, std::shared_ptr<std::atomic<uint32_t>> counter)
    : socket_(std::move(socket)), dataRead(0), counter_(counter) {
  }

  static size_t const BUF_SIZE = 4096;

  void start() {
    dataRead = 0;
    do_read();
  }

 private:
  void do_read() {
    auto self(shared_from_this());
    socket_.async_read_some(asio::buffer(data_ + dataRead, BUF_SIZE - dataRead),
        [this, self](std::error_code ec, std::size_t length) {
          if (!ec) {
            dataRead += length;
            if (dataRead < 4) {
              do_read();
            } else {
              uint32_t len;
              memcpy(&len, data_, 4);
              if (dataRead < 4 + len) {
                do_read();
              } else {
                // Actually work:
                CountWork* work = new CountWork([this, self]() { this->do_write(); });
                workerFarm.submit(work);
              }
            }
          } else {
            //std::cerr << "Connection closed." << std::endl;
            (*counter_)--;
          }
        });
  }

  void do_write() {
    auto self(shared_from_this());
    asio::async_write(socket_, asio::buffer(data_, dataRead),
        [this, self](std::error_code ec, std::size_t length) {
          if (!ec) {
            dataRead = 0;
            do_read();
          } else {
            std::cerr << "Error in write, bailing out!" << std::endl;
            (*counter_)--;
          }
        });
  }

  tcp::socket socket_;
  char data_[BUF_SIZE];
  size_t dataRead;
  std::shared_ptr<std::atomic<uint32_t>> counter_;
};

class server {

 public:
  server(std::vector<std::unique_ptr<asio::io_context>>& io_contexts,
         short port)
    : io_contexts_(io_contexts),
      acceptor_(*io_contexts[0], tcp::endpoint(tcp::v4(), port)),
      socket_(*io_contexts[0]) {
    for (size_t i = 0; i < io_contexts.size(); ++i) {
      counts_.push_back(std::make_shared<std::atomic<uint32_t>>(0));
    }
    do_accept();
  }

 private:
  void do_accept() {
    // Look for the next io_service to use:
    uint32_t low = counts_[0]->load();
    size_t lowpos = 0;
    for (size_t i = 1; i < counts_.size(); ++i) {
      uint32_t x = counts_[i]->load();
      if (x < low) {
        low = x;
        lowpos = i;
      }
    }
    tcp::socket newSocket(*io_contexts_[lowpos]);
    socket_ = std::move(newSocket);
    acceptor_.async_accept(
        socket_,
        [this, lowpos](std::error_code ec) {
          if (!ec) {
            (*this->counts_[lowpos])++;
            //std::cout << "Accepting new connection on thread " << lowpos
            //    << std::endl;
            std::make_shared<Connection>(std::move(this->socket_),
                                         this->counts_[lowpos])->start();
          }
          // And accept the next:
          do_accept();
        });
  }

  std::vector<std::unique_ptr<asio::io_context>>& io_contexts_;
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::vector<std::shared_ptr<std::atomic<uint32_t>>> counts_;
};

int main(int argc, char* argv[])
{
  try {
    if (argc != 5) {
      std::cerr << "Usage: server4 <port> <nriothreads> <nrthreads> <delay>\n";
      return 1;
    }

    int port = std::atoi(argv[1]);
    int nrIOThreads = std::atoi(argv[2]);
    int nrThreads = std::atoi(argv[3]);
    globalDelay = std::atol(argv[4]);
    std::cout << "Hello, using " << nrIOThreads << " IOthreads and "
      << nrThreads << " worker threads with a delay of " << globalDelay
      << std::endl;

    std::vector<std::unique_ptr<asio::io_context>> io_contexts;
    std::vector<asio::io_context::work> works;
    for (int i = 0; i < nrIOThreads; ++i) {
      io_contexts.emplace_back(std::unique_ptr<asio::io_context>(new asio::io_context()));
      works.emplace_back(*io_contexts.back());
    }

    server s(io_contexts, port);

    for (int i = 0; i < 10000; ++i) {
      CountWork* work = new CountWork([](){});
      workerFarm.submit(work);
    }
    
    // Start some threads:
    std::vector<std::thread> threads;
    for (int i = 1; i < nrIOThreads; i++) {
      threads.emplace_back([&io_contexts, i]() { io_contexts[i]->run(); });
    }
    for (int i = 0; i < nrThreads; ++i) {
      threads.emplace_back([&]() { workerFarm.run(); });
    }
    io_contexts[0]->run();   // Start accepting
    
    for (size_t i = 0; i < threads.size(); ++i) {
      threads[i].join();
    }
    
  }
  catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}