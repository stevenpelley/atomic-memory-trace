// Copyright (c) 2013 Steven Pelley
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so, 
// subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all 
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// drain_buffer.cpp
//
// the atomic tracer tends to block on writing to the file handle with
// all other threads blocking on the file handle lock.
// The result is that one thread tends to run at a time, interfering with
// thread interleaving.  Instead, we would like to buffer output to memory
// and then completely block while draining the buffer.  We can double
// buffer so that one buffer fills while the next is draining

#include <pthread.h>
#include <iostream>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

struct buffer_t {
  char *buf;
  int64_t capacity;
  int64_t end_cursor; // next location to insert

  char pad [64 - ( sizeof(char*) + 2*sizeof(int64_t) )];
};

// shared
buffer_t bufs [2];
pthread_barrier_t *bar;
bool eof;

// read from std in to the buffer
// until the buffer fills or reach EOF
// use (iteration % 2) buffer
void* reader(void *ptr) {
  setvbuf(stdin, (char*)NULL, _IOFBF, 1 << 24);
  for (int64_t i = 0; ; ++i) {
    int64_t idx = i % 2;
    buffer_t *buf = &bufs[idx];
    buf->end_cursor = 0;

    while (buf->end_cursor < buf->capacity && !feof(stdin)) {
      int64_t size = fread(&buf->buf[buf->end_cursor], 1, buf->capacity-buf->end_cursor, stdin);
      buf->end_cursor += size;
      assert(buf->end_cursor <= buf->capacity);
    }

    pthread_barrier_wait(bar);
    eof = feof(stdin);
    pthread_barrier_wait(bar);
    if (eof) return NULL;
  }
}

// drain the buffer, writing to std out from buffer
// use (iteration + 1) % 2 buffer
void* writer(void *ptr) {
  setvbuf(stdout, (char*)NULL, _IOFBF, 1 << 24);
  for (int64_t i = 0; ; ++i) {
    int64_t idx = (i+1) % 2;
    buffer_t *buf = &bufs[idx];

    int64_t start = 0;
    while (start < buf->end_cursor) {
      int64_t size = fwrite(&buf->buf[start], 1, buf->end_cursor-start, stdout);
      start += size;
    }
    assert(start == buf->end_cursor);
    if (eof) return NULL;

    // sync and make sure eof is consistent
    pthread_barrier_wait(bar);
    pthread_barrier_wait(bar);
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cout << "usage: drain_buffer <total_capacity>" << std::endl;
    assert(false);
  }
  int64_t total_capacity = atol(argv[1]);
  int64_t buffer_capacity = total_capacity / 2;
  assert(buffer_capacity > 0);

  eof = false;
  bar = new pthread_barrier_t();
  pthread_barrier_init(bar, NULL, 2);
  for (int64_t i = 0; i < 2; ++i) {
    bufs[i].buf = new char [buffer_capacity];
    bufs[i].capacity = buffer_capacity;
    bufs[i].end_cursor = 0;
  }

  pthread_t read_thread, write_thread;
  assert(pthread_create(&read_thread, NULL, reader, NULL) == 0);
  assert(pthread_create(&write_thread, NULL, writer, NULL) == 0);

  assert(pthread_join(read_thread, NULL) == 0);
  assert(pthread_join(write_thread, NULL) == 0);

  delete bar;
  for (int64_t i = 0; i < 2; ++i) {
    delete [] bufs[i].buf;
  }
}
