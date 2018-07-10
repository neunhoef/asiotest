#include <cstdlib>
#include <cstring>
#include <iostream>
#include "asio.hpp"

using asio::ip::tcp;
using asio::ip::basic_resolver;

size_t MSG_SIZE = 60;

uint64_t get_tick_count_ns ()
{
  auto now = std::chrono::high_resolution_clock::now();

  return now.time_since_epoch().count();
}

std::string prettyTime(uint64_t nanoseconds)
{
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
  std::vector<uint64_t> times;
  std::vector<std::unique_ptr<asio::io_context>> io_contexts;
  asio::ip::basic_resolver_results<tcp> resolved;

  uint64_t num_req_pre_thrd;
  uint64_t num_out_thrds;
  uint64_t num_in_thrds;
  uint64_t payload_size;
  uint64_t req_timer_us;
  uint64_t bundle_size;
  uint64_t total_requests;

  std::atomic<uint64_t> recevied_msgs;

  ClientContext(uint64_t total_requests) : times(total_requests, 0), total_requests(total_requests), recevied_msgs(0) {}
};

class Connection : public std::enable_shared_from_this<Connection>
{

  ClientContext &ctx;
  tcp::socket socket_;

  std::shared_ptr<uint8_t[]> recv_buffer;
  size_t recv_buffer_size;
  size_t recv_buffer_write_offset;
  size_t recv_buffer_read_offset;

public:
  Connection(ClientContext &ctx, int i) : ctx(ctx), socket_(*ctx.io_contexts[i % ctx.io_contexts.size()]) {
    asio::connect(socket_, ctx.resolved);

    recv_buffer.reset(new uint8_t[2048]);
    recv_buffer_size            = 2048;
    recv_buffer_write_offset    = 0;
    recv_buffer_read_offset     = 0;
  }

  void realloc_recv_buffer (std::size_t required_size)
  {
    size_t bytes_ahead = recv_buffer_size - recv_buffer_read_offset;

    if (required_size >= bytes_ahead)
    {
      // reallocation required
      size_t new_size = std::max(2048ul, required_size + 1024);
      uint8_t *new_buffer = new uint8_t[new_size];


      size_t bytes_to_copy = recv_buffer_write_offset - recv_buffer_read_offset;
      memcpy (new_buffer, recv_buffer.get() + recv_buffer_read_offset, bytes_to_copy);

      recv_buffer.reset(new_buffer);
      recv_buffer_read_offset   = 0;
      recv_buffer_write_offset  = bytes_to_copy;
      recv_buffer_size          = new_size;
    }
  }

  void do_read()
  {

    auto self(shared_from_this());

    auto buffer = asio::buffer(
      recv_buffer.get() + recv_buffer_write_offset,
      recv_buffer_size - recv_buffer_write_offset
    );

    auto _on_read = [this, self] (std::error_code ec, std::size_t bytes_read) {

      if (ec) {
        return ;
      }

      recv_buffer_write_offset += bytes_read;
      size_t bytes_free = recv_buffer_size - recv_buffer_write_offset;

      while (true)
      {
        size_t bytes_available = recv_buffer_write_offset
          - recv_buffer_read_offset;

        if (bytes_available > sizeof(uint32_t)) {
          // we can read the msg length
          uint32_t recv_msg_size;
          memcpy (&recv_msg_size, recv_buffer.get() + recv_buffer_read_offset, sizeof(uint32_t));
          bytes_available -= sizeof(uint32_t);

          if (bytes_available >= recv_msg_size) {
            // the whole msg is available
            recv_buffer_read_offset += sizeof(uint32_t);

            // get msg id
            uint64_t msg_id;
            memcpy (&msg_id, recv_buffer.get() + recv_buffer_read_offset, sizeof(uint64_t));

            //std::cout<<"Received msg "<<msg_id<<std::endl;

            ctx.times[msg_id] = get_tick_count_ns() - ctx.times[msg_id];

            uint64_t num_msgs = ctx.recevied_msgs.fetch_add(1) + 1;

            if (num_msgs == ctx.total_requests)
            {
              std::cout<<"All messages received."<<std::endl;
              // stop all
              for (size_t i = 0; i < ctx.io_contexts.size(); i++)
              {
                ctx.io_contexts[i]->stop();
              }

              return ;
            }

            recv_buffer_read_offset += recv_msg_size;
          } else {
            size_t realloc_size = recv_msg_size + sizeof(uint32_t);

            // msg not yet received, check if enough space is available
            if (bytes_free <= realloc_size)
            {
              // not enough memory available, realloc
              realloc_recv_buffer (realloc_size);
              do_read();
              return ;
            }

            break ;
          }
        } else {
          // next message length not received
          break ;
        }
      }

      // make sure there is a reasonable amout of free space available
      if (bytes_free < 100) {
        // not enough memory available, realloc
        realloc_recv_buffer (2048);
      }

      do_read();
    };

    socket_.async_read_some(buffer, _on_read);
  }

public:
  void generate_work (uint64_t msg_id) {
    uint32_t size = sizeof(uint64_t) + ctx.payload_size;

    uint8_t request[sizeof(uint32_t) + size];
    memcpy(request, &size, sizeof(uint32_t));    // set length

    // fill in message id

    memcpy(request + sizeof(uint32_t), &msg_id, sizeof(uint64_t));

    ctx.times[msg_id] = get_tick_count_ns();
    asio::write(socket_, asio::buffer(request, sizeof(uint32_t) + size));
  }
};

void do_out_work (ClientContext &ctx, uint64_t msg_id_start, int i) {
  try
  {
    /*
     *  Create a tcp socket and connect to the server.
     */
    auto connection = std::shared_ptr<Connection>(new Connection(ctx, i));

    connection->do_read();

    for (uint64_t j = 0; j < ctx.num_req_pre_thrd; ++j)
    {
      uint64_t msg_id = msg_id_start + j;
      connection->generate_work(msg_id);
      usleep(ctx.req_timer_us);
    }

  } catch (std::exception& e) {
    std::cerr << "Exception ("<<msg_id_start<<"): " << e.what() << "\n";
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

  try
  {

    if (argc != 9)
    {
      std::cerr << "Usage: asio_client_varlen <host> <port> <req_pre_thread>\n\t<num_out_threads> <num_in_threads> <payload> <req_timer us> <bundle_size>\n"
                << "Creates requests every <req_timer> us up to <req_pre_thread> pre thread. Each thread opens a new connection. \n"
                << "<bundle_size> controls how many reqs are packed into a single write call. The payload is at least 8 byte. Use <payload> to extend the payload size.\n";
      return 1;
    }

    uint64_t num_req_pre_thrd   = std::atol(argv[3]);
    uint64_t num_out_thrds      = std::atoi(argv[4]);
    uint64_t num_in_thrds       = std::atoi(argv[5]);
    uint64_t payload_size       = std::atoi(argv[6]);
    uint64_t req_timer_us       = std::atoi(argv[7]);
    uint64_t bundle_size        = std::atoi(argv[8]);


    uint64_t total_requests = num_req_pre_thrd * num_out_thrds;

    ClientContext ctx(total_requests);
    ctx.num_req_pre_thrd  = num_req_pre_thrd;
    ctx.num_out_thrds     = num_out_thrds;
    ctx.num_in_thrds      = num_in_thrds;
    ctx.payload_size      = payload_size;
    ctx.req_timer_us      = req_timer_us;
    ctx.bundle_size       = bundle_size;


    /*
     *  To handle received messages, use a worker farm.
     */



    // allocate one more io context for outgoing worker and resolver
    std::vector<asio::io_context::work> works;
    for (size_t i = 0; i < num_in_thrds + 1; ++i)
    {
      ctx.io_contexts.emplace_back(std::unique_ptr<asio::io_context>(new asio::io_context(1)));
      works.emplace_back(*ctx.io_contexts.back());
    }

    /*
     *  Resolve the target address.
     */
    tcp::resolver resolver(*ctx.io_contexts[0]);
    ctx.resolved = resolver.resolve(argv[1], argv[2]);

    auto startTime = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (unsigned int i = 1; i < num_in_thrds + 1; i++)
    {
      threads.emplace_back([i, &ctx]() { ctx.io_contexts[i]->run(); });
    }

    for (unsigned int i = 0; i < num_out_thrds; ++i) {
      threads.emplace_back([&ctx, i, num_req_pre_thrd]() {
        do_out_work(ctx, i * num_req_pre_thrd, i);
      });
    }

    ctx.io_contexts[0]->run();

    for (unsigned int i = 0; i < threads.size(); ++i) {
      threads[i].join();
    }

    auto endTime = std::chrono::high_resolution_clock::now();



    print_stats(ctx.times);

    uint64_t totalTime = std::chrono::nanoseconds(endTime - startTime).count();
    std::cout << "Total time : " << prettyTime(totalTime) << "\n";
    std::cout << "Reqs/s     : " << (double) total_requests * 1000000000.0 / totalTime
      << std::endl;

  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
