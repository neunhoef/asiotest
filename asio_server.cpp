//
// async_tcp_echo_server.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include "asio.hpp"

#include "asio_client_server.h"

using asio::ip::tcp;

uint64_t globalDelay = 1;

class session
  : public std::enable_shared_from_this<session>
{
public:
  session(tcp::socket socket)
    : socket_(std::move(socket)), dummy(0) {
  }

  uint64_t delayRunner(uint64_t delay) {
    for (uint64_t i = 0; i < delay; ++i) {
      dummy += i * i;
    }
    return dummy;
  }

  uint64_t getDummy() const {
    return dummy;
  }

  void start()
  {
    dataRead = 0;
    do_read();
  }

private:
  void do_read()
  {
    auto self(shared_from_this());
    socket_.async_read_some(asio::buffer(data_, MSG_SIZE),
        [this, self](std::error_code ec, std::size_t length)
        {
          if (!ec) {
            dataRead += length;
            if (dataRead < MSG_SIZE) {
              do_read();
            } else {
              data_[0] = (unsigned char) (delayRunner(globalDelay) & 0xff);
              do_write();
            }
          }
        });
  }

  void do_write()
  {
    auto self(shared_from_this());
    asio::async_write(socket_, asio::buffer(data_, MSG_SIZE),
        [this, self](std::error_code ec, std::size_t /*length*/)
        {
          if (!ec)
          {
            do_read();
          }
        });
  }

  tcp::socket socket_;
  char data_[MSG_SIZE];
  size_t dataRead;
  uint64_t dummy;
};

class server {

public:
  server(asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
  {
    do_accept();
  }

private:
  void do_accept()
  {
    acceptor_.async_accept(
        [this](std::error_code ec, tcp::socket socket)
        {
          if (!ec)
          {
            std::make_shared<session>(std::move(socket))->start();
          }

          do_accept();
        });
  }

  tcp::acceptor acceptor_;
};

void calibrate() {

}

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 4)
    {
      std::cerr << "Usage: async_tcp_echo_server <port> <nrthreads> <delay>\n";
      return 1;
    }

    asio::io_context io_context;

    server s(io_context, std::atoi(argv[1]));

    int nrThreads = std::atoi(argv[2]);
    globalDelay = std::atol(argv[3]);

    // Start some threads:
    std::vector<std::thread> threads;
    for (int i = 0; i < nrThreads-1; i++) {
      threads.emplace_back([&]() { io_context.run(); });
    }
    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
