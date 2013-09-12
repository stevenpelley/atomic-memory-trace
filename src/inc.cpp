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

// atomic inc
//
// given a single shared int variable have a number of threads:
// atomic_inc the variable, possibly waiting a small amount of time afterwards
// each thread should count the number of times the variable was even pre-inc
//
// all traced accesses to the counter should be atomic_fetchadd_long
// Use the memory trace to determine how many pre-inc evens were observed from each thread
// if number of pre-inc evens matches between the trace and the application output we have
// strong confidence that the trace is correct and atomic

#include <pthread.h>
#include <iostream>
#include <assert.h>
#include <stdint.h>
#include "annotation.h"

static __inline u_long
atomic_fetchadd_long(volatile u_long *p, u_long v) {
   __asm __volatile(
   "       lock ;                  "
   "       xaddq   %0,%1 ;         "
   "# atomic_fetchadd_long"
   : "+r" (v),                     /* 0 */
   "+m" (*p)                     /* 1 */
   : : "cc");
   return (v);
}

struct thread_data_t {
  pthread_barrier_t *bar1;
  pthread_barrier_t *bar2;
  pthread_barrier_t *bar3;
  pthread_barrier_t *bar4;
  uint64_t *shared;
  int64_t delay;

  int64_t threadid;
  int64_t count;
  int64_t even_count;
  int64_t stop_count;
};

struct counter_w {
  uint64_t counter;
  char padding [56];
};

void* thread_incs(void *ptr) {
  thread_data_t* tdata = reinterpret_cast<thread_data_t*>(ptr);
  uint64_t old_val = 0;

  atomic_trace::register_thread(tdata->threadid);
  pthread_barrier_wait(tdata->bar1);
  // ROI begins here
  pthread_barrier_wait(tdata->bar2);

  do {
    old_val = __sync_fetch_and_add(tdata->shared, 1);
    if (old_val % 2 == 0) { // inc'ed on even
      ++tdata->even_count;
    }
    ++tdata->count;
  } while (old_val < tdata->stop_count);

  pthread_barrier_wait(tdata->bar3);
  // ROI ends here
  pthread_barrier_wait(tdata->bar4);
  return NULL;
}

int main(int argc, char** argv) {
  int64_t num_threads;
  int64_t to_insert_total;
  int64_t delay;

  assert(argc == 3);
  num_threads = atoi(argv[1]);
  to_insert_total = atoi(argv[2]);

  assert(num_threads > 0);

  // threads will each increment once beyond to_insert_total
  // so subtract the number of threads from the insert total
  assert(to_insert_total > num_threads);
  to_insert_total -= num_threads;

  counter_w *counter = reinterpret_cast<counter_w*>(atomic_trace::special_malloc(sizeof(counter_w)));
  counter->counter = 0;

  pthread_barrier_t *bar1 = new pthread_barrier_t();
  pthread_barrier_t *bar2 = new pthread_barrier_t();
  pthread_barrier_t *bar3 = new pthread_barrier_t();
  pthread_barrier_t *bar4 = new pthread_barrier_t();
  pthread_barrier_init(bar1, NULL, num_threads+1);
  pthread_barrier_init(bar2, NULL, num_threads+1);
  pthread_barrier_init(bar3, NULL, num_threads+1);
  pthread_barrier_init(bar4, NULL, num_threads+1);
  thread_data_t *tdata = new thread_data_t[num_threads];
  pthread_t *threads = new pthread_t[num_threads];

  srand(time(NULL));

  for (int64_t i = 0; i < num_threads; ++i) {
    tdata[i].threadid = i;
    tdata[i].bar1 = bar1;
    tdata[i].bar2 = bar2;
    tdata[i].bar3 = bar3;
    tdata[i].bar4 = bar4;
    tdata[i].shared = &counter->counter;
    tdata[i].delay = delay;
    tdata[i].count = 0;
    tdata[i].even_count = 0;
    tdata[i].stop_count = to_insert_total;

    uint64_t ret = pthread_create(&threads[i], NULL, thread_incs, (void*) &tdata[i]);
  }

  pthread_barrier_wait(bar1);
  atomic_trace::start_roi();
  pthread_barrier_wait(bar2);

  pthread_barrier_wait(bar3);
  atomic_trace::end_roi();
  pthread_barrier_wait(bar4);

  for (int64_t i = 0; i < num_threads; ++i) {
    uint64_t ret = pthread_join(threads[i], NULL);
  }

  assert(counter->counter == to_insert_total+num_threads);

  // print out the count and even count for each thread
  for (int64_t i = 0; i < num_threads; ++i) {
    std::cout << "thread " << i << "\t" << tdata[i].count << "\t" << tdata[i].even_count << std::endl;
  }

  delete [] threads;
  delete [] tdata;
  delete bar1;
  delete bar2;
  delete bar3;
  delete bar4;

  atomic_trace::special_free(counter);
}
