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
#include "asio/ssl.hpp"

#include "pretty-time.hpp"

inline void cpu_relax() {
// TODO use <boost/fiber/detail/cpu_relax.hpp> when available (>1.65.0?)
#if defined(__i386) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(_M_X64)
#if defined _WIN32
  YieldProcessor();
#else
  asm volatile("pause" ::: "memory");
#endif
#else
  static constexpr std::chrono::microseconds us0{0};
  std::this_thread::sleep_for(us0);
#endif
}

static
void set_thread_name (const char *name) {
  pthread_setname_np(pthread_self(), name);
}


#include "adv-worker-farm.hpp"
#include "best-worker-farm.hpp"


std::atomic<uint64_t> post_time_counter [32];

using asio::ip::tcp;

class membuffer {
  uint8_t *ptr_;
  size_t size_;

public:
  class view {
    std::shared_ptr<membuffer> buffer_;
    size_t offset_, size_;
  public:
    view(std::shared_ptr<membuffer> &buffer, size_t offset, size_t size) :
      buffer_(buffer),
      offset_(offset),
      size_(size)
    {
      if (buffer_->size() < offset + size) {
        std::cout<<"buffer_size: "<<buffer_->size()<<" offset: "<<offset<<" size: "<<size<<std::endl;
        throw std::length_error("View out of bounds.");
      }
    }

    uint8_t *ptr() {
      return buffer_->ptr() + offset_;
    }

    size_t size() {
      return size_;
    }
  };

  membuffer (size_t size) : ptr_(new uint8_t[size]), size_(size) {}
  membuffer (uint8_t *ptr, size_t size) : ptr_(ptr), size_(size) {}
  ~membuffer() {
    if(ptr_) {
      free (ptr_);
    }
  }

  uint8_t *ptr() {
    return ptr_;
  }

  size_t size() {
    return size_;
  }
};

uint64_t globalDelay;

class AdvancedWork : public Work
{
public:
  typedef std::function<void(membuffer*)> on_completion_cb_t;

  std::shared_ptr<membuffer::view> request;

  on_completion_cb_t completion_;

  public:
    AdvancedWork (std::shared_ptr<membuffer::view> &request, on_completion_cb_t completion) :
      request(request),
      completion_(completion)
    {}

    void doit()
    {
      // parse the request
      // create response
      auto response = new membuffer(request->size() + sizeof(uint32_t));

      uint64_t delay = delayRunner(globalDelay);

      uint32_t request_size_32 = request->size();

      memcpy (response->ptr(), &request_size_32, sizeof(uint32_t));
      memcpy (response->ptr() + sizeof(uint32_t), request->ptr(), request->size());
      memcpy (response->ptr() + 3 * sizeof(uint32_t), &delay, sizeof(uint64_t));

      completion_(response);
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

template <typename T, typename self_type>
class socket_reader {


  std::shared_ptr<membuffer> buffer_;
  size_t write_offset_;
  size_t read_offset_;

  T &socket_;

public:
  typedef std::function<void (std::shared_ptr<membuffer::view>)> recv_cb;

  socket_reader(T &socket) :
    buffer_(std::make_shared<membuffer>(2048)),
    write_offset_(0),
    read_offset_(0),
    socket_(socket)
  {}

private:
  void realloc_recv_buffer (size_t required_size)
  {
    size_t bytes_ahead = buffer_->size() - write_offset_;

    if (required_size >= bytes_ahead)
    {
      // reallocation required
      size_t new_size = std::max(2048ul, required_size + 1024);
      uint8_t *new_buffer = new uint8_t[new_size];


      size_t bytes_to_copy = write_offset_ - read_offset_;
      memcpy (new_buffer, buffer_->ptr() + read_offset_, bytes_to_copy);

      buffer_.reset(new membuffer(new_buffer, new_size));
      read_offset_   = 0;
      write_offset_  = bytes_to_copy;
    }
  }

public:
  void do_read_socket(std::shared_ptr<self_type> self, recv_cb cb)
  {
    auto buffer = asio::buffer(
      buffer_->ptr() + write_offset_,
      buffer_->size() - write_offset_
    );

    socket_.async_read_some(buffer,
      [this, self, cb] (std::error_code ec, std::size_t bytes_read) {

      if (ec) {
        return ;
      }

      write_offset_ += bytes_read;
      size_t bytes_free = buffer_->size() - write_offset_;

      while (true)
      {
        size_t bytes_available = write_offset_
          - read_offset_;

        if (bytes_available > sizeof(uint32_t)) {
          // we can read the msg length
          uint32_t recv_msg_size;
          memcpy (&recv_msg_size, buffer_->ptr() + read_offset_, sizeof(uint32_t));
          bytes_available -= sizeof(uint32_t);

          if (bytes_available >= recv_msg_size) {
            // the whole msg is available
            read_offset_ += sizeof(uint32_t);
            cb (std::make_shared<membuffer::view>(buffer_, read_offset_, recv_msg_size));
            read_offset_ += recv_msg_size;

          } else {
            size_t realloc_size = recv_msg_size + sizeof(uint32_t);

            // msg not yet received, check if enough space is available
            if (bytes_free <= realloc_size)
            {
              // not enough memory available, realloc
              realloc_recv_buffer (realloc_size);
              do_read_socket(self, cb);
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

      do_read_socket(self, cb);
    });
  }
};


class server {

 public:
  struct io_context_t {
    asio::io_context asio_ioctx_;
    asio::io_context::work work_;
    std::atomic<uint32_t> clients_;
    std::thread thread_;

    // Create the asio io context and a thread that runs it
    io_context_t() :
      work_(asio_ioctx_),
      clients_(0),
      thread_([this](){
        set_thread_name("server-io");
        asio_ioctx_.run();
      })
    {}

    ~io_context_t() {
      stop();
    }

    void stop() {
      asio_ioctx_.stop();
    }

    void post(std::function<void()> cb) {
      asio_ioctx_.post(std::move(cb));
    }
  };

  template <typename S>
  class connection :
    public std::enable_shared_from_this<connection<S>>
  {
  public:
    virtual ~connection() {}
    virtual void start() = 0;
    virtual void post_response(membuffer *response) = 0;

    virtual S& lowest_layer() = 0;
  };

  template <typename T>
  class typed_connection :
    public connection<typename T::lowest_layer_type>
  {
    typedef connection<typename T::lowest_layer_type> connection_type;

    io_context_t &io_context_;
    std::atomic<bool> is_sending;

    boost::lockfree::queue<membuffer*> write_queue_;

  protected:
    T socket_;
    server &server_;

  private:
    socket_reader<T, connection<typename T::lowest_layer_type>> reader_;


   public:
    template <typename... SocketArgs>
    typed_connection(io_context_t &io_context, server &server, SocketArgs&&... args) :
      io_context_(io_context),
      is_sending(false),
      write_queue_(1000),
      socket_(args...),
      server_(server),
      reader_(socket_)
    {}

    ~typed_connection() {
      io_context_.clients_--;
    }

    virtual void start() {
      auto self(this->shared_from_this());

      reader_.do_read_socket(self, [this, self](std::shared_ptr<membuffer::view> view) {
        auto self2(this->shared_from_this());

        server_.workerfarm_->submit(
          new AdvancedWork(view, [this, self2](membuffer *response) {
            auto start = std::chrono::high_resolution_clock::now();
            this->post_response(response);
            auto end = std::chrono::high_resolution_clock::now();

            uint64_t time = std::chrono::nanoseconds(end - start).count(), level = 1000000000;

            for (int i = 0; i < 32; i++)
            {
              if (time > level) {
                post_time_counter[i].fetch_add(1, std::memory_order_relaxed);
                break ;
              }

              level /= 2;
            }
          })
        );
      });
    };

    void post_response(membuffer *response) {
      auto self(this->shared_from_this());

      write_queue_.push(response);

      if (!is_sending) {
        io_context_.post([this, self](){
          if (!is_sending) {
            is_sending = true;
            do_write_queue();
          }
        });
      }
    }

   private:
    bool do_write() {
      auto self(this->shared_from_this());

      membuffer *response;
      if (write_queue_.pop(response)) {
        asio::async_write(socket_, asio::buffer(response->ptr(), response->size()),
          [this, self, response](std::error_code ec, size_t bytes_written){
            if (!ec) {
              do_write_queue();
            }

            delete response;
          });
        return true;
      }

      return false;
    }

    void do_write_queue()
    {
      if (!do_write()) {
        is_sending = false;

        if (do_write()) {
          is_sending = true;
          return ;
        }
      }
    }

    typename T::lowest_layer_type& lowest_layer()
    {
      return socket_.lowest_layer();
    }
  };

  template <typename T>
  friend class typed_connection;

  class tcp_connection : public typed_connection<tcp::socket> {
  public:
    tcp_connection(io_context_t &io_context, server &s) :
      typed_connection(io_context, s, io_context.asio_ioctx_)
    {}
  };

  typedef asio::ssl::stream<asio::ip::tcp::socket> ssl_socket;

  class ssl_connection : public typed_connection<ssl_socket> {
  public:
    ssl_connection(io_context_t &io_context, server &s, asio::ssl::context &ssl_context) :
      typed_connection(io_context, s, io_context.asio_ioctx_, ssl_context)
    {}

    void start() {
      auto self(this->shared_from_this());
      socket_.async_handshake(asio::ssl::stream<asio::ip::tcp::socket>::server,
        [this, self](const std::error_code& ec) {
          if (ec) {
            std::cout<<"Handshake failure: "<<ec<<std::endl;
          } else {
            //std::cout<<conn_id<<": Handshake success "<<ec<<std::endl;
            typed_connection<ssl_socket>::start();
          }
        });
    }
  };

 private:
  std::vector<io_context_t> io_contexts_;

  tcp::acceptor acceptor_;
  std::shared_ptr<connection<tcp::socket::lowest_layer_type>> connection_;

  WorkerFarm *workerfarm_;
  asio::ssl::context *ssl_context_;

  size_t total_connections_;

 public:
  server (size_t num_io_threads, short port, WorkerFarm *workerfarm, asio::ssl::context *ssl_context) :
    io_contexts_(num_io_threads),
    acceptor_(io_contexts_[0].asio_ioctx_, tcp::endpoint(tcp::v4(), port)),
    workerfarm_(workerfarm),
    ssl_context_(ssl_context)
  {
    do_accept();
  }

  void stop() {
    for (auto &ctx : io_contexts_) {
      ctx.stop();
    }
  }

  void join() {
    for (auto &ctx : io_contexts_) {
      ctx.thread_.join();
    }
  }

  WorkerFarm *get_worker_farm() {
    return workerfarm_;
  }

 private:
  void do_accept() {
    // Look for the next io_service to use:
    uint32_t low = io_contexts_[0].clients_.load();
    size_t lowpos = 0;

    for (size_t i = 1; i < io_contexts_.size(); ++i) {
      uint32_t x = io_contexts_[i].clients_.load();
      if (x < low) {
        low = x;
        lowpos = i;
      }
    }

    auto &io_ctx = io_contexts_[lowpos];

    if (ssl_context_ != nullptr) {
      connection_ = std::make_shared<ssl_connection>(io_ctx, *this, *ssl_context_);
    } else {
      connection_ = std::make_shared<tcp_connection>(io_ctx, *this);
    }

    acceptor_.async_accept(connection_->lowest_layer(),
      [this, &io_ctx] (std::error_code ec) {

        if (!ec) {
          io_ctx.clients_ += 1;
          connection_->start();
        }

        // And accept the next:
        do_accept();
      });
  }
};

std::function<void ()> signal_callback;

void _signal_handler(int signal) {
  signal_callback();
}

int main(int argc, char* argv[])
{
  try {
    if (argc != 7) {
      std::cerr << "Usage: server4 <port> <num io threads> <num idle worker> <num max worker> <workload> <ssl?>"
        << std::endl;

      return EXIT_FAILURE;
    }

    int port            = std::atoi(argv[1]);
    int num_io_threads  = std::atoi(argv[2]);
    int num_idle_worker = std::atoi(argv[3]);
    int num_max_worker  = std::atoi(argv[4]);
    globalDelay         = std::atoi(argv[5]);
    int ssl             = std::atoi(argv[6]);

    std::cout
      << "IO Threads:     " << num_io_threads << std::endl
      << "Worker:         " << num_idle_worker << " upto " << num_max_worker << std::endl
      << "Sim. workload:  " << globalDelay << std::endl;

    for (int i = 0; i < 32; i++) {
      post_time_counter[i] = 0;
    }

    asio::ssl::context ssl_context(asio::ssl::context::sslv23);

    if (ssl) {
      std::cout<<"SSL/TLS enabled"<<std::endl;

      ssl_context.set_options(
          asio::ssl::context::default_workarounds
          | asio::ssl::context::no_sslv2
          | asio::ssl::context::single_dh_use);
      ssl_context.use_certificate_chain_file("cert.pem");
      ssl_context.use_private_key_file("key.pem", asio::ssl::context::pem);
    }

    AdvCleverWorkerFarm workerFarm(1000, num_max_worker, num_idle_worker);

    server s(num_io_threads, port, &workerFarm, ssl ? &ssl_context : nullptr);

    std::signal(SIGINT, _signal_handler);
    signal_callback = [&s]() {
        // Stop all io contexts
        s.stop();
    };




    s.join();
    workerFarm.stop();
    std::cout<<"Server terminated"<<std::endl;

    std::cout<<"post() wait times:"<<std::endl;
    uint64_t time = 1000000000;
    for (int i = 0; i < 32; i++) {
      std::cout<<prettyTime(time)<<": "<<post_time_counter[i]<<std::endl;
      time /= 2;

      if (time == 0) {
        break ;
      }
    }

  }
  catch (std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
