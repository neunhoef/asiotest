#ifndef ADV_WORK_H
#define ADV_WORK_H

uint64_t post_time_counter[32];

class AdvancedWork : public Work
{
public:
  typedef std::function<void(std::shared_ptr<BufferHolder>, size_t)> on_completion_cb_t;



  std::shared_ptr<BufferHolder> request_buffer;
  size_t request_size, request_offset;

  on_completion_cb_t completion_;

  public:
    AdvancedWork (std::shared_ptr<BufferHolder> &request, size_t request_offset,
      size_t request_size, on_completion_cb_t completion) :
      request_buffer(request), request_size(request_size), request_offset(request_offset), completion_(completion) {}

    void doit_stat(WorkerStat &stat)
    {
      stat.num_work++;


      // parse the request
      // create response
      uint8_t *response = (uint8_t*) malloc(request_size + sizeof(uint32_t));//new uint8_t[request_size + sizeof(uint32_t)];

      auto start = std::chrono::high_resolution_clock::now();
      uint64_t delay = delayRunner(globalDelay);
      auto end = std::chrono::high_resolution_clock::now();
      stat.work_time += std::chrono::nanoseconds(end - start).count();


      //uint64_t msg_id;
      //memcpy(&msg_id, request_buffer.get() + request_offset, sizeof(uint64_t));

      uint32_t request_size_32 = request_size;
      memcpy (response, &request_size_32, sizeof(uint32_t));
      memcpy (response + 3 * sizeof(uint32_t), &delay, sizeof(uint64_t));
      memcpy (response + sizeof(uint32_t), request_buffer->get() + request_offset, request_size);



      std::shared_ptr<BufferHolder> shared(new BufferHolder(response));

      start = std::chrono::high_resolution_clock::now();
      completion_(shared, request_size + sizeof(uint32_t));
      end = std::chrono::high_resolution_clock::now();

      uint64_t time = std::chrono::nanoseconds(end - start).count(), level = 1000000000;

      for (int i = 0; i < 32; i++)
      {
        if (time > level) {
          post_time_counter[i]++;
          break ;
        }

        level /= 2;
      }

      stat.post_time += time;
    }

    void doit()
    {

      // parse the request
      // create response
      uint8_t *response = new uint8_t[request_size + sizeof(uint32_t)];

      uint64_t delay = delayRunner(globalDelay);

      //uint64_t msg_id;
      //memcpy(&msg_id, request_buffer.get() + request_offset, sizeof(uint64_t));

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

#endif
