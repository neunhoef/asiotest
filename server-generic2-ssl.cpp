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

#include <boost/bind.hpp>

#include "asio.hpp"
#include "asio/ssl.hpp"

#include "futex_worker_farm.h"
#include "richard_worker_farm.h"
#include "lockfree_richard_worker_farm.h"
#include "std_worker_farm.hpp"
#include "fire_forget_worker_farm.hpp"
#include "buffer_holder.hpp"
//#include "smart_buffer.hpp"

using asio::ip::tcp;

WorkerFarm *workerFarm;

uint64_t globalDelay = 0;
typedef asio::ssl::stream<asio::ip::tcp::socket> ssl_socket;

class AdvancedWork : public Work
{
public:
  typedef std::function<void(std::shared_ptr<BufferHolder>, size_t)> on_completion_cb_t;



  std::shared_ptr<BufferHolder> request_buffer;
  size_t request_size, request_offset;

  on_completion_cb_t completion_;

  public:
    AdvancedWork (std::shared_ptr<BufferHolder> request, size_t request_offset,
      size_t request_size, on_completion_cb_t completion) :
      request_buffer(request), request_size(request_size), request_offset(request_offset), completion_(completion) {}

    void doit()
    {
      // parse the request
      // create response
      uint8_t *response = new uint8_t[request_size + sizeof(uint32_t)];

      uint64_t delay = delayRunner(globalDelay);

      uint64_t msg_id;
      memcpy(&msg_id, request_buffer->get() + request_offset, sizeof(uint64_t));
      //std::cout<<"Working "<<msg_id<<std::endl;

      uint32_t request_size_32 = request_size;
      memcpy (response, &request_size_32, sizeof(uint32_t));
      memcpy (response + 3 * sizeof(uint32_t), &delay, sizeof(uint64_t));
      memcpy (response + sizeof(uint32_t), request_buffer->get() + request_offset, request_size);

      std::shared_ptr<BufferHolder> shared(new BufferHolder(response));

      completion_(shared, request_size + sizeof(uint32_t));
    }

  private:
  uint64_t delayRunner(uint64_t delay) {
    uint64_t dummy_ = 0;
    for (uint64_t i = 0; i < delay; ++i) {
      dummy_ += i * i;
    }
    return dummy_;
  }
};

class Connection : public std::enable_shared_from_this<Connection>
{
  ///*asio::ssl::stream<*/asio::ip::tcp::socket/*>*/ socket_;
  asio::ssl::stream<asio::ip::tcp::socket> socket_;
  asio::io_context& context_;

  asio::io_context::strand strand_;

  std::shared_ptr<std::atomic<uint32_t>> counter_;



  std::shared_ptr<BufferHolder> recv_buffer;
  size_t recv_buffer_size;
  size_t recv_buffer_write_offset;
  size_t recv_buffer_read_offset;

  int conn_id, total_sent, total_recvd;

  std::deque<std::tuple<std::shared_ptr<BufferHolder>, size_t>> write_queue_;
  bool write_pending;


public:
  Connection(int conn_id, asio::io_context& io_context, asio::ssl::context& ssl_context, std::shared_ptr<std::atomic<uint32_t>> counter)
    :
    socket_(io_context/**/, ssl_context/**/),
    context_(io_context),
    strand_(io_context),
    counter_(counter),
    conn_id(conn_id),
    total_sent(0),
    total_recvd(0),
    write_pending(false)
    {
  }

  ~Connection()
  {
    //std::cout<<conn_id<<": "<<"Closing connection"<<std::endl;
  }

  void start() {

    auto self(shared_from_this());

    recv_buffer.reset(new BufferHolder(new uint8_t[2048]));
    recv_buffer_size            = 2048;
    recv_buffer_write_offset    = 0;
    recv_buffer_read_offset     = 0;
    try {
      /*socket_.handshake(asio::ssl::stream<asio::ip::tcp::socket>::server);*/
      strand_.post([this, self]() {
        auto self_strand(shared_from_this());

        socket_.async_handshake(asio::ssl::stream<asio::ip::tcp::socket>::server,
          strand_.wrap([this, self_strand](const std::error_code& ec) {


            if (ec) {
              std::cout<<conn_id<<": Handshake failure "<<ec<<std::endl;
            } else {
              //std::cout<<conn_id<<": Handshake success "<<ec<<std::endl;
              do_read();
            }
          }));
      });
    } catch (std::exception& e) {
      std::cerr <<conn_id<<": "<< "Exception: " << e.what() << "\n";
    }
  }

  ssl_socket::lowest_layer_type& socket()
  {
    return socket_.lowest_layer();
  }


 private:

  void realloc_recv_buffer (std::size_t required_size)
  {
    size_t bytes_ahead = recv_buffer_size - recv_buffer_read_offset;

    if (required_size >= bytes_ahead)
    {
      // reallocation required
      size_t new_size = std::max(2048ul, required_size + 1024);
      uint8_t *new_buffer = new uint8_t[new_size];


      size_t bytes_to_copy = recv_buffer_write_offset - recv_buffer_read_offset;
      memcpy (new_buffer, recv_buffer->get() + recv_buffer_read_offset, bytes_to_copy);

      recv_buffer.reset(new BufferHolder(new_buffer));
      recv_buffer_read_offset   = 0;
      recv_buffer_write_offset  = bytes_to_copy;
      recv_buffer_size          = new_size;
    }
  }

  void do_read()
  {
    //std::cout<<conn_id<<": "<<"do_read setup"<<std::endl;

    auto self(shared_from_this());

    auto buffer = asio::buffer(
      recv_buffer->get() + recv_buffer_write_offset,
      recv_buffer_size - recv_buffer_write_offset
    );

    auto _on_read = [this, self] (std::error_code ec, std::size_t bytes_read) {

      if (ec) {
        //std::cout<<conn_id<<": "<<"Read error "<<ec<<std::endl;
        return ;
      }

      recv_buffer_write_offset += bytes_read;
      size_t bytes_free = recv_buffer_size - recv_buffer_write_offset;

      while (true)
      {
        size_t bytes_available = recv_buffer_write_offset
          - recv_buffer_read_offset;

        //std::cout<<conn_id<<": "<<"Bytes available: "<<bytes_available<<std::endl;

        if (bytes_available > sizeof(uint32_t)) {
          // we can read the msg length
          uint32_t recv_msg_size;
          memcpy (&recv_msg_size, recv_buffer->get() + recv_buffer_read_offset, sizeof(uint32_t));
          bytes_available -= sizeof(uint32_t);

          if (bytes_available >= recv_msg_size) {
            // the whole msg is available
            recv_buffer_read_offset += sizeof(uint32_t);

            uint64_t msg_id;
            memcpy(&msg_id, recv_buffer->get() + recv_buffer_read_offset, sizeof(uint64_t));
            //std::cout<<conn_id<<": Received msg "<<msg_id<<std::endl;

            total_recvd++;

            // pass the message to the worker
            AdvancedWork *work = new AdvancedWork(recv_buffer, recv_buffer_read_offset, recv_msg_size,
              [this, self] (std::shared_ptr<BufferHolder> response, size_t response_size) { this->do_write(response, response_size); });
            workerFarm->submit(work);

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

    socket_.async_read_some(buffer, strand_.wrap(_on_read));
  }

  void do_do_write(std::shared_ptr<BufferHolder> response, size_t response_size) {

    auto self(shared_from_this());

    asio::async_write(socket_, asio::buffer(response->get(), response_size),
      [this, self, response](std::error_code ec, std::size_t length) {
        if (ec) {
          (*counter_)--;
          std::cout<<conn_id<<": "<<"Socket write error"<<std::endl;
        } else {
          uint64_t msg_id;
          memcpy (&msg_id, response->get() + sizeof(uint32_t), sizeof(uint64_t));

          total_sent++;

          //std::cout<<conn_id<<": "<<"Sent msg "<<msg_id<<" size: "<<length<<std::endl;
          //std::cout<<"\t"<<total_sent<<" of "<<total_recvd<<" recvd"<<std::endl;

          if (write_queue_.size() != 0) {
            auto next_write = write_queue_.front();
            write_queue_.pop_front();

            do_do_write(std::get<0>(next_write), std::get<1>(next_write));
          } else {
            write_pending = false;
          }
        }
      });
  }

  void do_write(std::shared_ptr<BufferHolder> response, size_t response_size) {
    // Note that this is typically called by a thread that is not in a
    // run() method of this io_context. Therefore, to make sure that this
    // all works with SSL later, we have to make sure that the async_write
    // is actually executed on the thread that is run()ing in the io_context!
    auto self(shared_from_this());

    uint64_t msg_id;
    memcpy (&msg_id, response->get() + sizeof(uint32_t), sizeof(uint64_t));

    //std::cout<<conn_id<<": "<<"enqueueing msg "<<msg_id<<std::endl;

    strand_.post(
      [this, self, response, response_size]() {


        uint64_t msg_id;
        memcpy (&msg_id, response->get() + sizeof(uint32_t), sizeof(uint64_t));

        //std::cout<<conn_id<<": "<<"writing msg "<<msg_id<<std::endl;

        if (write_pending) {
          write_queue_.emplace_back(response, response_size);
        } else {
          write_pending = true;
          do_do_write(response, response_size);
        }
      });
  }


};

class server {

 public:
  server(std::vector<std::unique_ptr<asio::io_context>>& io_contexts,
         short port)
    : io_contexts_(io_contexts),
      acceptor_(*io_contexts[0], tcp::endpoint(tcp::v4(), port)),
      context_(asio::ssl::context::sslv23),
      conn_count(0) {
    for (size_t i = 0; i < io_contexts.size(); ++i) {
      counts_.push_back(std::make_shared<std::atomic<uint32_t>>(0));
    }

    context_.set_options(
        asio::ssl::context::default_workarounds
        | asio::ssl::context::no_sslv2
        | asio::ssl::context::single_dh_use);
    context_.set_password_callback(boost::bind(&server::get_password, this));

    context_.use_certificate_chain_file("cert.pem");
    context_.use_private_key_file("key.pem", asio::ssl::context::pem);

    do_accept();
  }

  std::string get_password() const
  {
    return "test";
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

    auto conn = std::make_shared<Connection>(conn_count++, *io_contexts_[lowpos], context_, this->counts_[lowpos]);

    acceptor_.async_accept(
        conn->socket(),
        [this, lowpos, conn](std::error_code ec) {
          if (!ec) {
            (*this->counts_[lowpos])++;
            conn->start();
          }
          // And accept the next:
          do_accept();
        });
  }

  std::vector<std::unique_ptr<asio::io_context>>& io_contexts_;
  tcp::acceptor acceptor_;
  std::vector<std::shared_ptr<std::atomic<uint32_t>>> counts_;
  asio::ssl::context context_;
  int conn_count;
};

std::function<void(int)> sigusr1_handler;

void _sigusr1_handler(int signal) {
  sigusr1_handler(signal);
}

enum impl_enum {
  impl_richard_lock = 1,
  impl_futex = 2,
  impl_richard_lock_free = 3,
  impl_std_mutex = 4,
  impl_richard_a2 = 5
};

int main(int argc, char* argv[])
{
  try {
    if (argc != 6) {
      std::cerr << "Usage: server4 <port> <nriothreads> <nrthreads> <delay> <impl>\n"
        << "Implementations: 1 - Richard, 2 - Futex (Manuel), 3 - Lockfree Richard, 4 - Std Lockfree, 5 - Richard's A2\n";
      return 1;
    }

    std::srand(std::time(nullptr));

    int impl = std::atoi(argv[5]);
    int port = std::atoi(argv[1]);
    int nrIOThreads = std::atoi(argv[2]);
    int nrThreads = std::atoi(argv[3]);
    globalDelay = std::atol(argv[4]);
    std::cout << "Hello, using " << nrIOThreads << " IOthreads and "
      << nrThreads << " worker threads with a delay of " //<< globalDelay
      << std::endl;

    switch (impl)
    {
      case impl_richard_lock:
        std::cout<<"Testing using Richards impl. (with locks)"<<std::endl;
        workerFarm = new RichardWorkerFarm(100000);
        break ;
      case impl_futex:
        std::cout<<"Testing using Futex impl."<<std::endl;
        workerFarm = new FutexWorkerFarm(100000);
        break ;
      case impl_richard_lock_free:
        std::cout<<"Testing using Richards impl. (lock free)"<<std::endl;
        workerFarm = new LockfreeRichardWorkerFarm(100000);
        break ;
      case impl_std_mutex:
        std::cout<<"Testing std. mutex worker farm"<<std::endl;
        workerFarm = new StdWorkerFarm(10000);
        break ;
      case impl_richard_a2:
      default:
        std::cout<<"Testing std. mutex worker farm"<<std::endl;
        workerFarm = new FireForgetWorkerFarm(10000);
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
      std::cout<< i << " sleeps: " << stats[i].num_sleeps << " work_num: " << stats[i].num_work << " w/s: " << stats[i].num_work / stats[i].num_sleeps
        << " avg. work_time: " <<
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
