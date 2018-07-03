#include <cstdlib>
#include <cstring>
#include <iostream>
#include "asio.hpp"

using asio::ip::tcp;

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

void do_work(asio::io_context* io_context, std::string hostname, std::string port, size_t tries, uint64_t* times) {
  tcp::socket s(*io_context);
  tcp::resolver resolver(*io_context);
  asio::connect(s, resolver.resolve(hostname.c_str(), port.c_str()));

  uint32_t size = MSG_SIZE;
  char request[MSG_SIZE + 4];
  memcpy(request, &size, 4);    // set length
  for (size_t i = 0; i < MSG_SIZE-1; ++i) {
    request[i+4] = 'x';
  }
  request[MSG_SIZE-1] = 0;

  std::chrono::high_resolution_clock clock;
  for (uint64_t j = 0; j < tries; ++j) {
    auto startTime = clock.now();
    asio::write(s, asio::buffer(request, MSG_SIZE + 4));

    char reply[MSG_SIZE + 4];
    size_t reply_length = 0;
    while (reply_length < MSG_SIZE + 4) {
      reply_length += asio::read(s,asio::buffer(reply, MSG_SIZE + 4 - reply_length));
    }
    auto endTime = clock.now();
    times[j] = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();
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
  /*std::cout << "Statistics:\n";
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
  }*/

  std::cout<<"smpls="<<nr<<" avg="<<prettyTime(sum / nr)<<" med="<<prettyTime(times[nr/2]);
}

int main(int argc, char* argv[]) {
  try {
    if (argc != 6)
    {
      std::cerr << "Usage: asio_client_varlen <host> <port> <tries> <nrthreads> <msg_size>\n";
      return 1;
    }

    asio::io_context io_context;

    uint64_t nrtries = std::atol(argv[3]);
    unsigned int nrthreads = std::atoi(argv[4]);
    MSG_SIZE = std::atoi(argv[5]);

    // Make nrtries divisible by nrthreads:
    nrtries = (((nrtries-1) / nrthreads) + 1) * nrthreads;
    std::vector<uint64_t> times(nrtries, 0);

    auto startTime = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < nrthreads; ++i) {
      threads.emplace_back(do_work, &io_context, argv[1], argv[2],
        nrtries / nrthreads, times.data() + i * nrtries / nrthreads);
    }
    for (unsigned int i = 0; i < nrthreads; ++i) {
      threads[i].join();
    }
    auto endTime = std::chrono::high_resolution_clock::now();
    print_stats(times);
    uint64_t totalTime = std::chrono::nanoseconds(endTime - startTime).count();
    //std::cout << "Total time : " << prettyTime(totalTime) << "\n";
    std::cout << " reqs/s=" << (double) nrtries * 1000000000.0 / totalTime;
  } catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
