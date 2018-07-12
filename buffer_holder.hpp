#ifndef BUFFER_HOLDER_H
#define BUFFER_HOLDER_H

class BufferHolder
{
  uint8_t *_buffer;

public:
  BufferHolder(uint8_t *buffer) : _buffer(buffer) {}
  ~BufferHolder() {
    if (_buffer) {
      free (_buffer);
    }
  }

  uint8_t *get() {
    return _buffer;
  }

  //operator uint8_t*() const { return _buffer; }
};


#endif
