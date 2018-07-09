#include <cstdlib>
#include <cstring>
#include <iostream>
#include "asio.hpp"

using asio::ip::tcp;
using asio::ip::basic_resolver;

size_t MSG_SIZE = 60;

std::string prettyTime(uint64_t nanoseconds) {
  if (nanoseconds < 10000) {
    return std::to_string(nanoseconds) + " ns";
  } else if (nanoseconds < 10000000) {
    return std::to_string((nanoseconds / 10) / 100.0) + " us";
  } else if (nanoseconds < 10000000000) {
    return std::to_string((nanoseconds / 10000) / 100.0) + " ms";
  } else {
    return std::to_string((nanoseconds / 10000000) / 100.0) + " s";
  }
}

struct ClientContext
{
  uint64_t *times;
  uint64_t _counter;
  uint32_t msg_size;
  uint8_t *msg;

  std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> start_times;

  ClientContext(uint64_t *times, int tries) : times(times), _counter(tries), start_times(tries) {}
};

void do_receive(tcp::socket &s, ClientContext &ctx)
{
  asio::async_read(s, asio::buffer(&ctx.msg_size, sizeof(uint32_t)),
    [&s, &ctx](std::error_code ec, std::size_t length) {

      if (ec) {
        std::cout<<"Connection error"<<std::endl;
      }

      ctx.msg = new uint8_t[ctx.msg_size];

      asio::async_read(s, asio::buffer(ctx.msg, ctx.msg_size),
        [&s, &ctx](std::error_code ec, std::size_t length) {

          if (ec) {
            std::cout<<"Connection error"<<std::endl;
          }

          uint64_t msg_id;
          memcpy (&msg_id, ctx.msg, sizeof(uint64_t));

          auto now = std::chrono::high_resolution_clock::now();

          ctx.times[msg_id] = std::chrono::duration_cast<std::chrono::nanoseconds>
            (now - ctx.start_times[msg_id]).count();


          delete [] ctx.msg;

          if(--ctx._counter == 0) {
            s.get_io_context().stop();
          };

          do_receive(s, ctx);
        });
    });
}

void do_work(asio::ip::basic_resolver_results<tcp> resolved, size_t tries, uint64_t* times, size_t payload, uint64_t req_timer) {
  try {
    asio::io_context io_context;
    tcp::socket s(io_context);
    asio::connect(s, resolved);

    uint32_t size = sizeof(uint64_t) + payload;
    uint8_t request[sizeof(uint32_t) + size];
    memcpy(request, &size, 4);    // set length

    /*
     *  Generate a request every 20us. When a message is recevied, read it.
     */
    ClientContext ctx(times, tries);

    do_receive (s, ctx);

    for (uint64_t j = 0; j < tries; ++j) {
      // fill in message id
      memcpy(request + 4, &j, sizeof(uint64_t));

      ctx.start_times[j] = std::chrono::high_resolution_clock::now();
      asio::write(s, asio::buffer(request, sizeof(uint32_t) + size));

      io_context.run_for(std::chrono::microseconds(req_timer));
    }

    // now wait for all request to return
    io_context.run();

  } catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

}

void print_stats(std::vector<uint64_t> times) {
  size_t nr = times.size();
  if (nr == 0) {
    return;
  }
  std::sort(times.begin(), times.end());
  uint64_t sum = 0;
  for (auto& t : times) {
    sum += t;
  }
  std::cout << "Statistics:\n";
  std::cout << "Samples : " << nr << "\n";
  std::cout << "Average : " << prettyTime(sum / nr) << "\n";
  std::cout << "Median  : " << prettyTime(times[nr/2]) << "\n";
  std::cout << "90%     : " << prettyTime(times[(nr*90)/100]) << "\n";
  std::cout << "99%     : " << prettyTime(times[(nr*99)/100]) << "\n";
  std::cout << "99.9%   : " << prettyTime(times[(nr*999)/1000]) << "\n";
  if (nr >= 20) {
    std::string s;
    for (size_t i = 0; i < 10; ++i) {
      s = s + "  " + prettyTime(times[i]) + "\n";
    }
    std::cout << "Smallest:\n" << s;
    s.clear();
    for (size_t i = 10; i > 0; --i) {
      s = s + "  " + prettyTime(times[nr-i]) + "\n";
    }
    std::cout << "Largest:\n" << s;
  }
}

int main(int argc, char* argv[]) {
  try {
    if (argc != 6)
    {
      std::cerr << "Usage: asio_client_varlen <host> <port> <tries> <nrthreads> <payload> <req_timer us>\n";
      return 1;
    }

    asio::io_context io_context;
    tcp::resolver resolver(io_context);

    auto resolved = resolver.resolve(argv[1], argv[2]);

    std::srand(std::time(nullptr));

    uint64_t nrtries      = std::atol(argv[3]);
    uint64_t nrthreads    = std::atoi(argv[4]);
    uint64_t payload_size = std::atoi(argv[5]);
    uint64_t req_timer    = std::atoi(argv[6]);

    // Make nrtries divisible by nrthreads:
    nrtries = (((nrtries-1) / nrthreads) + 1) * nrthreads;
    std::vector<uint64_t> times(nrtries, 0);

    auto startTime = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < nrthreads; ++i) {
      threads.emplace_back(do_work, resolved, nrtries / nrthreads,
        times.data() + i * nrtries / nrthreads, payload_size, req_timer);
    }

    for (unsigned int i = 0; i < nrthreads; ++i) {
      threads[i].join();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    print_stats(times);

    uint64_t totalTime = std::chrono::nanoseconds(endTime - startTime).count();
    std::cout << "Total time : " << prettyTime(totalTime) << "\n";
    std::cout << "Reqs/s     : " << (double) nrtries * 1000000000.0 / totalTime
      << std::endl;

  } catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
