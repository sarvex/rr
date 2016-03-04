/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "CompressedWriter"

#define _LARGEFILE64_SOURCE

#include "CompressedWriter.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

using namespace std;

void* CompressedWriter::compression_thread_callback(void* p) {
  static_cast<CompressedWriter*>(p)->compression_thread();
  return nullptr;
}

CompressedWriter::CompressedWriter(const string& filename, size_t block_size,
                                   uint32_t num_threads)
    : fd(filename.c_str(),
         O_CLOEXEC | O_WRONLY | O_CREAT | O_EXCL | O_LARGEFILE, 0400) {
  this->block_size = block_size;
  threads.resize(num_threads);
  thread_pos.resize(num_threads);
  buffer.resize(block_size * (num_threads + 2));
  pthread_mutex_init(&mutex, nullptr);
  pthread_cond_init(&cond, nullptr);

  for (uint32_t i = 0; i < num_threads; ++i) {
    thread_pos[i] = UINT64_MAX;
  }
  next_thread_pos = 0;
  next_thread_end_pos = 0;
  closing = false;
  write_error = false;

  producer_reserved_pos = 0;
  producer_reserved_write_pos = 0;
  producer_reserved_upto_pos = 0;
  error = false;
  if (fd < 0) {
    error = true;
    return;
  }

  // Hold the lock so threads don't inspect the 'threads' array
  // until we've finished initializing it.
  pthread_mutex_lock(&mutex);
  for (uint32_t i = 0; i < num_threads; ++i) {
    pthread_create(&threads[i], nullptr, compression_thread_callback, this);
    size_t last_slash = filename.rfind('/');
    string thread_name =
        string("compress ") + (last_slash == string::npos
                                   ? filename
                                   : filename.substr(last_slash + 1));
    pthread_setname_np(threads[i], thread_name.substr(0, 15).c_str());
  }
  pthread_mutex_unlock(&mutex);
}

CompressedWriter::~CompressedWriter() {
  close();
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}

void CompressedWriter::write(const void* data, size_t size) {
  while (!error && size > 0) {
    uint64_t reservation_size =
        producer_reserved_upto_pos - producer_reserved_write_pos;
    if (reservation_size == 0) {
      update_reservation(WAIT);
      continue;
    }
    size_t buf_offset = (size_t)(producer_reserved_write_pos % buffer.size());
    size_t amount = min(buffer.size() - buf_offset,
                        (size_t)min<uint64_t>(reservation_size, size));
    memcpy(&buffer[buf_offset], data, amount);
    producer_reserved_write_pos += amount;
    data = static_cast<const char*>(data) + amount;
    size -= amount;
  }

  if (!error &&
      producer_reserved_write_pos - producer_reserved_pos >=
          buffer.size() / 2) {
    update_reservation(NOWAIT);
  }
}

void CompressedWriter::update_reservation(WaitFlag wait_flag) {
  pthread_mutex_lock(&mutex);

  next_thread_end_pos = producer_reserved_write_pos;
  producer_reserved_pos = producer_reserved_write_pos;

  // Wake up threads that might be waiting to consume data.
  pthread_cond_broadcast(&cond);

  while (!error) {
    if (write_error) {
      error = true;
      break;
    }

    uint64_t completed_pos = next_thread_pos;
    for (uint32_t i = 0; i < thread_pos.size(); ++i) {
      completed_pos = min(completed_pos, thread_pos[i]);
    }
    producer_reserved_upto_pos = completed_pos + buffer.size();
    if (producer_reserved_pos < producer_reserved_upto_pos ||
        wait_flag == NOWAIT) {
      break;
    }

    pthread_cond_wait(&cond, &mutex);
  }

  pthread_mutex_unlock(&mutex);
}

void CompressedWriter::compression_thread() {
  pthread_mutex_lock(&mutex);

  int thread_index;
  pthread_t self = pthread_self();
  for (thread_index = 0; threads[thread_index] != self; ++thread_index) {
  }

  // Add slop for incompressible data
  vector<uint8_t> outputbuf;
  outputbuf.resize((size_t)(block_size * 1.1) + sizeof(BlockHeader));
  BlockHeader* header = reinterpret_cast<BlockHeader*>(&outputbuf[0]);

  while (true) {
    if (!write_error && next_thread_pos < next_thread_end_pos &&
        (closing || next_thread_pos + block_size <= next_thread_end_pos)) {
      thread_pos[thread_index] = next_thread_pos;
      next_thread_pos = min(next_thread_end_pos, next_thread_pos + block_size);
      // header->uncompressed_length must be <= block_size,
      // therefore fits in a size_t.
      header->uncompressed_length =
          (size_t)(next_thread_pos - thread_pos[thread_index]);

      pthread_mutex_unlock(&mutex);
      header->compressed_length =
          do_compress(thread_pos[thread_index], header->uncompressed_length,
                      &outputbuf[sizeof(BlockHeader)],
                      outputbuf.size() - sizeof(BlockHeader));
      pthread_mutex_lock(&mutex);

      if (header->compressed_length == 0) {
        write_error = true;
      }

      // wait until we're the next thread that needs to write
      while (!write_error) {
        bool other_thread_write_first = false;
        for (uint32_t i = 0; i < thread_pos.size(); ++i) {
          if (thread_pos[i] < thread_pos[thread_index]) {
            other_thread_write_first = true;
          }
        }
        if (!other_thread_write_first) {
          break;
        }
        pthread_cond_wait(&cond, &mutex);
      }

      if (!write_error) {
        pthread_mutex_unlock(&mutex);
        ::write(fd, &outputbuf[0],
                sizeof(BlockHeader) + header->compressed_length);
        pthread_mutex_lock(&mutex);
      }

      thread_pos[thread_index] = UINT64_MAX;
      // do a broadcast because we might need to unblock
      // the producer thread or a compressor thread waiting
      // for us to write.
      pthread_cond_broadcast(&cond);
      continue;
    }

    if (closing && (write_error || next_thread_pos == next_thread_end_pos)) {
      break;
    }

    pthread_cond_wait(&cond, &mutex);
  }

  pthread_mutex_unlock(&mutex);
}

void CompressedWriter::close() {
  if (!fd.is_open()) {
    return;
  }

  update_reservation(NOWAIT);

  pthread_mutex_lock(&mutex);
  closing = true;
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&mutex);

  for (auto i = threads.begin(); i != threads.end(); ++i) {
    pthread_join(*i, nullptr);
  }

  fd.close();
}

size_t CompressedWriter::do_compress(uint64_t offset, size_t length,
                                     uint8_t* outputbuf, size_t outputbuf_len) {
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  int result = deflateInit(&stream, Z_DEFAULT_COMPRESSION);
  if (result != Z_OK) {
    assert(0 && "deflateInit failed!");
    return 0;
  }

  stream.next_out = outputbuf;
  stream.avail_out = outputbuf_len;

  while (length > 0 || stream.avail_in > 0) {
    if (stream.avail_in == 0) {
      size_t buf_offset = (size_t)(offset % buffer.size());
      size_t amount = min(length, buffer.size() - buf_offset);
      stream.next_in = &buffer[buf_offset];
      stream.avail_in = amount;
      length -= amount;
      offset += amount;
    }
    if (stream.avail_out == 0) {
      assert(0 && "outputbuf exhausted!");
      return 0;
    }
    result = deflate(&stream, length == 0 ? Z_FINISH : Z_NO_FLUSH);
    if (result != (length == 0 ? Z_STREAM_END : Z_OK)) {
      assert(0 && "deflate failed!");
      return 0;
    }
  }

  result = deflateEnd(&stream);
  if (result != Z_OK) {
    assert(0 && "deflateEnd failed!");
    return 0;
  }

  return stream.total_out;
}
