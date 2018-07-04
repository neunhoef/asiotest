#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <deque>
#include "asio.hpp"
#include "lockfree_richard_worker_farm.h"

using asio::ip::tcp;



LockfreeRichardWorkerFarm workerFarm(10000000);

uint64_t globalDelay = 1;

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
                auto self(shared_from_this());
                CountWork* work
                  = new CountWork([this, self]() { this->do_write(); }, globalDelay);
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

std::function<void(int)> sigusr1_handler;

void _sigusr1_handler(int signal) { 
  sigusr1_handler(signal); 
}

int main(int argc, char* argv[])
{
  try {
    if (argc != 5) {
      std::cerr << "Usage: server3 <port> <nriothreads> <nrthreads> <delay>\n";
      return 1;
    }

    int port = std::atoi(argv[1]);
    int nrIOThreads = std::atoi(argv[2]);
    int nrThreads = std::atoi(argv[3]);
    globalDelay = std::atol(argv[4]);
    //std::cerr << "Hello, using " << nrIOThreads << " IOthreads and "
    //  << nrThreads << " worker threads with a delay of " << globalDelay
    //  << std::endl;

    std::vector<std::unique_ptr<asio::io_context>> io_contexts;
    std::vector<asio::io_context::work> works;
    for (int i = 0; i < nrIOThreads; ++i) {
      io_contexts.emplace_back(std::unique_ptr<asio::io_context>(new asio::io_context()));
      works.emplace_back(*io_contexts.back());
    }

    server s(io_contexts, port);

    std::signal(SIGUSR2, _sigusr1_handler);
    sigusr1_handler = [&](int signal) {
        // Stop all io contexts
        for (int i = 0; i < nrIOThreads; i++) {
          io_contexts[i]->stop();
        }

        // stop worker farm
        workerFarm.stop();
    };

    // Start some threads:
    std::vector<std::thread> threads;
    for (int i = 1; i < nrIOThreads; i++) {
      threads.emplace_back([&io_contexts, i]() { io_contexts[i]->run(); });
    }

    std::vector<WorkerStat> stats(nrThreads);
    for (int i = 0; i < nrThreads; ++i) {
      threads.emplace_back([i, &stats]() { workerFarm.run(stats[i]); });
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
