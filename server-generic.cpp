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
#include "asio.hpp"
#include "futex_worker_farm.h"
#include "richard_worker_farm.h"
#include "lockfree_richard_worker_farm.h"
#include "std_worker_farm.hpp"

using asio::ip::tcp;

WorkerFarm *workerFarm;

uint64_t globalDelay = 1;



class Connection : public std::enable_shared_from_this<Connection> {
public:
  Connection(tcp::socket socket, std::shared_ptr<std::atomic<uint32_t>> counter)
    : socket_(std::move(socket)), context_(socket_.get_io_context()),
    dataRead(0), counter_(counter) {
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
                CountWork* work = new CountWork([this, self]() { this->do_write(); }, globalDelay);
                workerFarm->submit(work);
              }
            }
          } else {
            //std::cerr << "Connection closed." << std::endl;
            (*counter_)--;
          }
        });
  }

  void do_write() {
    // Note that this is typically called by a thread that is not in a
    // run() method of this io_context. Therefore, to make sure that this
    // all works with SSL later, we have to make sure that the async_write
    // is actually executed on the thread that is run()ing in the io_context!
    auto self(shared_from_this());
    context_.post(
      [this, self]() {
        auto self2(shared_from_this());
        asio::async_write(socket_, asio::buffer(data_, dataRead),
          [this, self2](std::error_code ec, std::size_t length) {
            if (!ec) {
              dataRead = 0;
              do_read();
            } else {
              std::cerr << "Error in write, bailing out!" << std::endl;
              (*counter_)--;
            }
          });
      });
  }




  tcp::socket socket_;
  asio::io_context& context_;
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

std::function<void(int)> sigusr1_handler;

void _sigusr1_handler(int signal) {
  sigusr1_handler(signal);
}

enum impl_enum {
  impl_richard_lock = 1,
  impl_futex = 2,
  impl_richard_lock_free = 3,
  impl_std_mutex = 4
};

int main(int argc, char* argv[])
{
  try {
    if (argc != 6) {
      std::cerr << "Usage: server4 <port> <nriothreads> <nrthreads> <delay> <impl>\n"
        << "Implementations: 1 - Richard, 2 - Futex (Manuel), 3 - Lockfree Richard, 4 - Std Lockfree\n";
      return 1;
    }

    int impl = std::atoi(argv[5]);
    int port = std::atoi(argv[1]);
    int nrIOThreads = std::atoi(argv[2]);
    int nrThreads = std::atoi(argv[3]);
    globalDelay = std::atol(argv[4]);
    std::cout << "Hello, using " << nrIOThreads << " IOthreads and "
      << nrThreads << " worker threads with a delay of " << globalDelay
      << std::endl;

    switch (impl)
    {
      case impl_richard_lock:
        std::cout<<"Testing using Richards impl. (with locks)"<<std::endl;
        workerFarm = new RichardWorkerFarm(100000000);
        break ;
      case impl_futex:
        std::cout<<"Testing using Futex impl."<<std::endl;
        workerFarm = new FutexWorkerFarm(100000000);
        break ;
      case impl_richard_lock_free:
        std::cout<<"Testing using Richards impl. (lock free)"<<std::endl;
        workerFarm = new LockfreeRichardWorkerFarm(100000000);
        break ;
      case impl_std_mutex:
      default:
        std::cout<<"Testing std. mutex worker farm"<<std::endl;
        workerFarm = new StdWorkerFarm(100000000);
        break ;
    }

    std::vector<std::unique_ptr<asio::io_context>> io_contexts;
    std::vector<asio::io_context::work> works;
    for (int i = 0; i < nrIOThreads; ++i) {
      io_contexts.emplace_back(std::unique_ptr<asio::io_context>(new asio::io_context()));
      works.emplace_back(*io_contexts.back());
    }

    server s(io_contexts, port);

    std::signal(SIGINT, _sigusr1_handler);
    sigusr1_handler = [&](int signal) {
        // Stop all io contexts
        for (int i = 0; i < nrIOThreads; i++) {
          io_contexts[i]->stop();
        }

        std::cout<<"Signaled stop."<<std::endl;
    };

    // Start some threads:
    std::vector<std::thread> threads;
    for (int i = 1; i < nrIOThreads; i++) {
      threads.emplace_back([&io_contexts, i]() { pthread_setname_np(pthread_self(), "server-io"); io_contexts[i]->run(); });
    }

    std::vector<WorkerStat> stats(nrThreads);
    for (int i = 0; i < nrThreads; ++i) {
      threads.emplace_back([i, &stats]() { pthread_setname_np(pthread_self(), "server-worker"); workerFarm->run(stats[i]); });
    }

    std::cout<<"Server up."<<std::endl;
    io_contexts[0]->run();   // Start accepting

    // wait for the IO threads to finish their job
    for (int i = 0; i < nrIOThreads - 1; ++i) {
      threads[i].join();
    }

    std::cout<<"IO Threads done. Wait for farm."<<std::endl;
    workerFarm->stop();

    // now wait for the worker threads to end
    for (size_t i = nrIOThreads - 1; i < threads.size(); ++i) {
      threads[i].join();
    }

    double totalTime = 0;
    double totalWork = 0;

    // Print worker statistics
    for (int i = 0; i < nrThreads; i++) {
      std::cout<< i << " sleeps: " << stats[i].num_sleeps << " work_num: " << stats[i].num_work << " work_time: " << stats[i].work_time << "ns avg. work_time: " <<
        stats[i].work_time / (1000.0 * stats[i].num_work) << "ns avg. w/s: "
        << (int) (1000000000.0 * stats[i].num_work / stats[i].work_time) <<  std::endl;
      totalTime += stats[i].work_time;
      totalWork += stats[i].num_work;
    }

    std::cout<<"Avg. Work: "<<  totalWork / nrThreads << " Work/Sec: "<< 1000000000 * totalWork / ( totalTime / nrThreads) <<std::endl;

    if (impl == impl_std_mutex) {
      StdWorkerFarm *stdWorkerFarm = (StdWorkerFarm*) workerFarm;

      std::cout<<"Max. Queue Length: "<<stdWorkerFarm->_queueMaxLength<<std::endl;
    }

    delete workerFarm;

  }
  catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
