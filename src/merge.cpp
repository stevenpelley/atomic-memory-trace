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

// merge.cpp
// merge the output of the atomic trace
// by lamport timestamp, strip the timestamp, and output
// the result.
//
// Break the input stream into thread components,
// assert that the threads are monotonic increasing in timestamp.
//
// We can only move entries to the output once we are certain no
// earlier timestamp is going to show up from another thread.
// this happens once we have observed at least that timestamp
// by every other thread.  Entries from unregistered threads (-1)
// must synchronize first, so it is not possible to pop a timestamp
// from any registered thread and then later pop an earlier timestamp
// from an unregistered thread.  Assert this as well.

#include <stdint.h>
#include <assert.h>
#include <iostream>
#include <string.h>

#include <string>
#include <queue>
#include <map>
#include <queue>

#include <boost/lexical_cast.hpp>
#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/multiset_of.hpp>

#if TRACE_DEBUG
  #define DO_DEBUG(S) S
#else
  #define DO_DEBUG(S) /* */
#endif

/////////////////////////////////////////////
// queue for each thread
// tracks timestamp, string (trace line), and length of string for each
// tracks whether the thread is finished
/////////////////////////////////////////////
class thread_queue_t {
 public:

  thread_queue_t();
  ~thread_queue_t();

  // enqueue the string in buffer of length with timestamp
  // return if the queue had been empty
  bool enqueue(int64_t timestamp, const char *buffer, int64_t length);

  // pop an entry into buffer (must be at least 256 bytes)
  // and put the string's length into length and timestamp into timestamp
  // return true if had not been empty and data is valid
  bool dequeue(int64_t *timestamp, char *buffer, int64_t *length);

  // return next timestamp or -1 if empty
  int64_t peek();
  
  void finish() {_thread_finished = true;}
  bool is_finished() {return _thread_finished;}

 private:
  static const int64_t _initial_capacity = 1024*1024; // 1mb per thread
  void _enlargen_char_buffer();
  void _assert_repinv();

  typedef std::pair<int64_t, int64_t> time_size_t;
  typedef std::queue<time_size_t> time_size_queue_t;

  // holds string traces associated with this thread
  char   *_buffer;
  int64_t _buffer_capacity;
  int64_t _buffer_size;
  int64_t _buffer_next_insert;
  int64_t _buffer_next_pop;

  // holds (timestamp, size) for each entry in char_stream
  time_size_queue_t _time_size_queue;

  int64_t _max_timestamp;
  bool _thread_finished;
};

thread_queue_t::thread_queue_t()
  : _buffer(new char[_initial_capacity])
  , _buffer_capacity(_initial_capacity)
  , _buffer_size(0)
  , _buffer_next_insert(0)
  , _buffer_next_pop(0)
  , _time_size_queue()
  , _max_timestamp(0)
  , _thread_finished(false)
{}

thread_queue_t::~thread_queue_t() {
  delete [] _buffer;
}

bool thread_queue_t::enqueue(int64_t timestamp, const char *buffer, int64_t length) {
  DO_DEBUG(_assert_repinv());
  assert(timestamp > _max_timestamp);
  assert(!_thread_finished);

  bool ret = _time_size_queue.empty();
  _max_timestamp = timestamp;

  int64_t space_left = _buffer_capacity - _buffer_size;
  // if we won't have enough room move to a larger buffer
  if (length > space_left) {
    _enlargen_char_buffer();
  }

  // may have to wrap, so do in 2 copies:
  // 1: from next_insert to end
  // 2: from front of buffer
  int64_t copy1_idx = _buffer_next_insert;
  int64_t copy1_len = std::min(_buffer_capacity - copy1_idx, length);
  int64_t copy2_len = length - copy1_len;
  memcpy(&_buffer[copy1_idx], buffer, copy1_len);
  memcpy(_buffer, &buffer[copy1_len], copy2_len);

  // update buffer counters
  _buffer_size += length;
  _buffer_next_insert = (_buffer_next_insert + length) % _buffer_capacity;

  _time_size_queue.push( time_size_t(timestamp, length) );

  DO_DEBUG(_assert_repinv());
  return ret;
}

bool thread_queue_t::dequeue(int64_t *timestamp, char *buffer, int64_t *length) {
  DO_DEBUG( _assert_repinv());
  assert(timestamp);
  assert(buffer);
  assert(length);

  if (_time_size_queue.empty()) return false;

  time_size_t time_size = _time_size_queue.front();
  _time_size_queue.pop();
  *timestamp = time_size.first;
  *length = time_size.second;


  // copy from next_pop to the end
  // copy remaining from the beginning
  int64_t copy1_idx = _buffer_next_pop;
  int64_t copy1_len = std::min(*length, _buffer_capacity - copy1_idx);
  int64_t copy2_len = *length - copy1_len;
  memcpy(buffer, &_buffer[copy1_idx], copy1_len);
  memcpy(&buffer[copy1_len], _buffer, copy2_len);

  _buffer_size -= *length;
  _buffer_next_pop = (_buffer_next_pop + *length) % _buffer_capacity;
  DO_DEBUG(_assert_repinv());

  return true;
}

int64_t thread_queue_t::peek() {
  if (_time_size_queue.empty()) return -1;
  return _time_size_queue.front().first;
}

// grow the buffer
void thread_queue_t::_enlargen_char_buffer() {
  DO_DEBUG(_assert_repinv());
  int64_t new_capacity = 4 * _buffer_capacity;
  char *new_buffer = new char[new_capacity];

  int64_t copy1_len = std::min(_buffer_size, _buffer_capacity - _buffer_next_pop);
  int64_t copy2_len = _buffer_size - copy1_len;

  memcpy(new_buffer, &_buffer[_buffer_next_pop], copy1_len);
  memcpy(&new_buffer[copy1_len], _buffer, copy2_len);
  delete [] _buffer;
  _buffer = new_buffer;
  _buffer_next_pop = 0;
  _buffer_next_insert = _buffer_size;
  _buffer_capacity = new_capacity;
  DO_DEBUG(_assert_repinv());
}

void thread_queue_t::_assert_repinv() {
  // check that the sum of sizes in _time_size_queue
  // match _buffer_size
  // queues do not support iteration so copy and pop
  time_size_queue_t copy_q(_time_size_queue);
  int64_t size = 0;
  while (!copy_q.empty()) {
    size += copy_q.front().second;
    copy_q.pop();
  }
  assert(size == _buffer_size);
}

/////////////////////////////////////////////
// holds queue per active thread
/////////////////////////////////////////////
typedef std::pair<int64_t, thread_queue_t*> threadid_queue_t;
typedef std::map<int64_t, thread_queue_t*> thread_queue_map_t;
thread_queue_map_t thread_queue_map;

/////////////////////////////////////////////
// utility class for priority queue that holds (time, threadid) tuples
// and returns the lowest time
/////////////////////////////////////////////

typedef std::pair<int64_t, int64_t> time_threadid_t;
class compare_first_greater_t  {
 public:
  bool operator() (const time_threadid_t &x, time_threadid_t &y) const {
    return x.first > y.first;
  }
};

/////////////////////////////////////////////
// returns the minimum trace read between active threads
/////////////////////////////////////////////

std::priority_queue<
                     time_threadid_t,
                     std::vector<time_threadid_t>,
                     compare_first_greater_t
                   > time_threadid_min;

/////////////////////////////////////////////
// returns the minimum of each threads' greatest observed time
// any trace less than this cannot yet be merged because a lower
// timestamp can still appear
/////////////////////////////////////////////

class safe_timestamp_t {
 public:
  safe_timestamp_t();
  int64_t timestamp();
  void set_time(int64_t threadid, int64_t timestamp);
  void finish_thread(int64_t threadid);

 private:
  int64_t _safe_timestamp_cache;
  typedef std::pair<int64_t, int64_t> threadid_timestamp_t;
  typedef std::vector<threadid_timestamp_t> tt_list_t;
  tt_list_t _tt_list;

  // compare by threadid in a list of (threadid, timestamp)
  class tt_compare_t {
   public:
    int64_t arg;
    int64_t set_arg(int64_t new_arg) {arg = new_arg;}
    bool operator() (threadid_timestamp_t &tt){
      return tt.first == arg;
    }
  };

  void _update_cache();
} safe_timestamp;

safe_timestamp_t::safe_timestamp_t()
  : _safe_timestamp_cache(0)
  , _tt_list()
{}

int64_t safe_timestamp_t::timestamp() {
  return _safe_timestamp_cache;
}

void safe_timestamp_t::set_time(int64_t threadid, int64_t timestamp) {
  static tt_compare_t tt_comp;
  tt_comp.set_arg(threadid);
  tt_list_t::iterator it = std::find_if(_tt_list.begin(), _tt_list.end(), tt_comp);
  bool need_update = false;
  if (it == _tt_list.end()) {
    _tt_list.push_back(threadid_timestamp_t(threadid, timestamp));
    need_update = true;
  } else {
    need_update = it->second <= _safe_timestamp_cache;
    (*it) = threadid_timestamp_t(threadid, timestamp);
  }

  if (need_update) {
    _update_cache();
  }
}

void safe_timestamp_t::finish_thread(int64_t threadid) {
  static tt_compare_t tt_comp;
  tt_comp.set_arg(threadid);
  tt_list_t::iterator it = std::find_if(_tt_list.begin(), _tt_list.end(), tt_comp);
  assert(it != _tt_list.end());
  _tt_list.erase(it);
  _update_cache();
}

void safe_timestamp_t::_update_cache() {
  tt_list_t::iterator it = _tt_list.begin();

  if (it == _tt_list.end()) {
    return;
  } else {
    int64_t m = it->second;
    for (; it != _tt_list.end(); ++it) {
      m = std::min(m, it->second);
    }
    _safe_timestamp_cache = m;
  }
}

/////////////////////////////////////////////
// helper functions and main for merging
/////////////////////////////////////////////

// parse the line, determining if it is an important type of trace
// pos1 is the position of the first tab (for cutting out the timestamp
void parse_line(const char* buf, int64_t *pos1, int64_t *timestamp, int64_t *threadid, bool *is_thread_register, bool *is_thread_finish, bool *is_sync) {
  const char *tab1 = strchr(buf, '\t');
  assert(tab1);
  *pos1 = tab1 - buf;
  const char *tab2 = strchr(&buf[*pos1+1], '\t');
  assert(tab2);
  int64_t pos2 = tab2 - buf;

  *timestamp = atoll(buf);
  *threadid = atoll(tab1+1);

  // tr, tf, and thread_sync terminate the line
  *is_thread_register = strcmp("tr", tab2+1) == 0;
  *is_thread_finish = strcmp("tf", tab2+1) == 0;
  *is_sync = strcmp("thread_sync", tab2+1) == 0;
}

int64_t active_threads = 0;

// merge as many as allowable
// can merge so long as minimum timestamp entry is less/equal min of all
// threads' max observed timestamp
void merge() {
  static char buf[128]; // temp space used for merging
  bool keep_merging = true;
  while (keep_merging) {
    time_threadid_t min_entry = time_threadid_min.top();

    bool any_threads_active = active_threads > 0;
    int64_t all_threads_observed = safe_timestamp.timestamp();
    keep_merging = !any_threads_active || min_entry.first <= all_threads_observed;

    // put the minimum entry into the output and pop it
    if (keep_merging) {
      time_threadid_min.pop();

      // pop and output from min_entry.second
      thread_queue_map_t::iterator it = thread_queue_map.find(min_entry.second);
      assert(it != thread_queue_map.end());
      thread_queue_t *q = it->second;

      int64_t popped_time, popped_length;
      bool not_empty = q->dequeue(&popped_time, buf, &popped_length);
      assert(not_empty);
      assert(popped_time == min_entry.first);
      std::cout.write(buf, popped_length);
      std::cout << std::endl;

      int64_t next_time = q->peek();
      if (next_time == -1 && q->is_finished()) {
        delete q;
        thread_queue_map.erase(it);
      }
      // peek the next item from that threadid
      if (next_time >= 0) {
        time_threadid_min.push(time_threadid_t(next_time, min_entry.second));
      }
    }
    keep_merging = keep_merging && !time_threadid_min.empty();
  }
}

int main(int argc, char **argv) {
  bool keep_timestamps = argc > 1 && std::string(argv[1]) == "-t";
  std::string line;
  int64_t line_count = 0;
  char buf[128];
  while (std::cin.getline(buf, 128)) {
    int64_t len = strlen(buf);
    ++line_count;

    int64_t pos1, timestamp, threadid;
    bool is_thread_register, is_thread_finish, is_sync;
    parse_line(buf, &pos1, &timestamp, &threadid, &is_thread_register, &is_thread_finish, &is_sync);

    if (is_thread_register) {
      ++active_threads;
    } else if (is_thread_finish) {
      --active_threads;
      safe_timestamp.finish_thread(threadid);
    }

    // update thread's max observed if registered thread
    if (!is_thread_finish && threadid >= 0) {
      safe_timestamp.set_time(threadid, timestamp);
      if (is_sync) continue;
    }

    // get/construct the threadid's queue to add this to
    thread_queue_t* thread_queue;
    thread_queue_map_t::iterator it = thread_queue_map.find(threadid);
    if (it == thread_queue_map.end()) {
      thread_queue = new thread_queue_t();
      thread_queue_map.insert(it, threadid_queue_t(threadid, thread_queue));
    } else {
      thread_queue = it->second;
    }

    // strip out the timestamp from the string as it is enqueued
    bool was_empty;
    if (keep_timestamps) {
      was_empty = thread_queue->enqueue(timestamp, buf, len);
    } else {
      was_empty = thread_queue->enqueue(timestamp, buf+pos1+1, len - (pos1+1));
    }

    if (is_thread_finish) thread_queue->finish();

    // if thread had been empty update heap of each thread's min available entry
    if (was_empty) {
      time_threadid_min.push(time_threadid_t(timestamp, threadid));
    }
    
    // try to merge some entries
    merge();
  }
  assert(time_threadid_min.empty()); // should be no more to merge
}

