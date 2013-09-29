//Copyright (c) 2013 Steven Pelley
//
//Permission is hereby granted, free of charge, to any person obtaining a copy of
//this software and associated documentation files (the "Software"), to deal in
//the Software without restriction, including without limitation the rights to
//use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
//the Software, and to permit persons to whom the Software is furnished to do so, 
//subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all 
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
//FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
//COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
//IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
//CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// atomic memory trace pintool
// see README for directions and implementation details.

#include <string>
#include <algorithm>
#include "pin.H"
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdint.h>
#include <vector>
#include <set>
#include <assert.h>
#include <boost/algorithm/string.hpp>
#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/multiset_of.hpp>

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */

// helper class to define critical sections
// allocate on the stack within a scope (curly braces) for the CS
// constructor will acquire the lock, destructor (called when CS object
// goes out of scope) releases the lock
class pin_critical_section {
 public:
  pin_critical_section(PIN_MUTEX *lock)
  : _lock(lock), _paused(false), _pred(true) {
    PIN_MutexLock(_lock);
  }

  // predicated CS.  Only do it predicate is true
  pin_critical_section(PIN_MUTEX *lock, bool predicate)
  : _lock(lock), _paused(false), _pred(predicate) {
    if (_pred) {
      PIN_MutexLock(_lock);
    }
  }

  ~pin_critical_section() {
    if (_pred && !_paused) {
      PIN_MutexUnlock(_lock);
    }
  }

  void pause() {
    if (_pred && !_paused) {
      PIN_MutexUnlock(_lock);
    }
  }

  // make sure not to violate the lock hierarchy if you use this!
  void restart() {
    if (_pred && _paused) {
      PIN_MutexLock(_lock);
    }
  }

 private:
  pin_critical_section();
  PIN_MUTEX *_lock;
  bool _paused;
  bool _pred;
};

bool in_roi;
bool require_roi;
bool register_threads = false;
bool turn_off_locks = false;

// global lock covers the file buffer, thread count, etc
//   may not be held beyond analysis function/callback return
//   i.e., critical section must be contained in function
struct lock_wrapper_t {
  PIN_MUTEX lock;
  char padding [56];
};

// bimap of <pin_threadid, last flushed timestamp>
typedef boost::bimap< 
                 boost::bimaps::set_of<int64_t>,
                 boost::bimaps::multiset_of<int64_t>
                 > last_flushed_t;

// file access
lock_wrapper_t file_lock;
std::ofstream trace_file;
last_flushed_t last_flushed; // pin threadid to last_flushed
int64_t last_flushed_cache; // occassionally computed min flushed

// thread tracking
lock_wrapper_t thread_start_fini_lock;
int64_t num_threads = 0;
set<int64_t> pin_threadid_set;

TLS_KEY tls_key;

int64_t num_locks = 0;
int64_t block_size = 0;
int64_t block_size_log = 0;
int64_t accesses_flush = 0;
int64_t timestamp_difference = 0;

// address locks allows atomic tracing
//   may be held beyond analysis function return
//   may not be held beyond callback functino return
//   must be released on context change
struct address_lock_t {
  PIN_MUTEX lock;
  int64_t lamport_timestamp;
  char padding [48];
};

address_lock_t *address_lock_bank;

/* ===================================================================== */
/* TLS Variables */
/* ===================================================================== */

class thread_data_t;

thread_data_t* get_tls(THREADID threadid) {
  thread_data_t *tdata =
    static_cast<thread_data_t*>(PIN_GetThreadData(tls_key, threadid));
  return tdata;
}


class thread_data_t {
 public:
  thread_data_t()
  :   user_threadid(-1)
    , event_count(0)
    , memory_instruction_count(0)
    , index_lock_list()
    , buffered_entries(0)
    , lamport_timestamp(0)
  {
    PIN_MutexInit(&thread_lock);
    index_lock_list.reserve(32);
  }

  // must hold thread_start_fini_lock
  // will either hold no thread locks or all thread locks
  //
  // try to flush threads in flush_others
  // if their timestamp is still below the threshold
  // (threads may race and flush their own buffer first)
  void attempt_flush_others(int64_t this_time, bool hold_all_thread_locks) {
    vector<int64_t>::iterator it;
    for (it = flush_others.begin(); it != flush_others.end(); ++it) {
      int64_t pin_threadid = *it;
      set<int64_t>::iterator set_it = pin_threadid_set.find(pin_threadid);
      if (set_it != pin_threadid_set.end()) {
        thread_data_t *tdata = get_tls(pin_threadid);
        {
          pin_critical_section CS(&tdata->thread_lock, !hold_all_thread_locks);
          int64_t diff = this_time - tdata->lamport_timestamp;
          if (diff >= timestamp_difference * .75) { // sync it!
            if (tdata->user_threadid >= 0) {
               tdata->trace_stream << this_time << "\t" << tdata->user_threadid << "\tthread_sync" << "\n";
               ++tdata->buffered_entries;
            }
            tdata->lamport_timestamp = this_time;

            if (tdata->buffered_entries > 0) { // unregistered thread might be empty
              pin_critical_section CS2(&file_lock.lock);
              trace_file << tdata->trace_stream.rdbuf();
              assert(trace_file);
              tdata->buffered_entries = 0;
              tdata->trace_stream.str(std::string());
              last_flushed.left.erase(pin_threadid);
              last_flushed.left.insert(std::pair<int64_t, int64_t>(pin_threadid, this_time));
            }
          }
        }
      }
    }
  }

  // after having written something to the string buffer:
  // update timestamp
  // increment entry count
  // flush the buffer if necessary
  // flush buffers of any thread lagging too far behind
  //
  // requires either:
  // thread_start_fini_lock not held, this thread's thread_lock held
  // or
  // thread_start_fini_lock and all thread_locks held
  //
  // if force always flush this thread's buffer
  void touch_buffer(THREADID pin_threadid, int64_t time, bool force, pin_critical_section *thread_cs, bool have_all_thread_locks) {
    lamport_timestamp = time;
    flush_others.clear();

    if (++buffered_entries >= accesses_flush || force) {
      pin_critical_section CS(&file_lock.lock);
      trace_file << trace_stream.rdbuf();
      assert(trace_file);
      buffered_entries = 0;
      // clear the trace_stream to use it as a queue
      trace_stream.str(std::string());

      // bimap must erase and insert, no modifying
      last_flushed.left.erase(pin_threadid);
      last_flushed.left.insert(std::pair<int64_t, int64_t>(pin_threadid, time));

      // double check last_flushed_cache and possibly update it
      // check for any other threads that should be flushed
      if (time - last_flushed_cache > timestamp_difference) {
        last_flushed_t::right_const_iterator it = last_flushed.right.begin();
        last_flushed_cache = it->first;
        // double check
        if (time - last_flushed_cache > timestamp_difference) {
          // flush any thread who differs by more than .75*timestamp_difference
          for ( ; it != last_flushed.right.end(); ++it) {
            int64_t diff = time - it->first;
            if (diff >= timestamp_difference * .75) {
              flush_others.push_back(it->second);
            } else {
              // cannot be end (because this thread has a high timestamp)
              last_flushed_cache = it->first;
              break;
            }
          }
        }
      }
    }

    // get all the thread locks in the proper order and check-and-flush
    // the other threads
    if (!flush_others.empty()) {
      if (!have_all_thread_locks) {
        thread_cs->pause();
        {
          pin_critical_section CS(&thread_start_fini_lock.lock);
          attempt_flush_others(time, have_all_thread_locks);
        }
        thread_cs->restart();
      } else {
        attempt_flush_others(time, have_all_thread_locks);
      }
    }
  }

  int64_t user_threadid; // -1 implies not a registered thread
  int64_t event_count;
  int64_t memory_instruction_count;

  // record which locks we hold (by index)
  vector<int64_t> index_lock_list;

  // TLS for memory access tracing
  bool trace_this_access;
  bool is_read;
  bool is_read2;
  bool is_write;
  int64_t read_size;
  int64_t write_size;
  uint64_t read1_address;
  uint64_t read2_address;
  uint64_t write_address;

  vector<int64_t> flush_others;

  // the following are covered by this lock
  int8_t padding [64];
  PIN_MUTEX thread_lock;

  stringstream trace_stream;
  int64_t buffered_entries;
  int64_t lamport_timestamp;

  int8_t padding2 [64];
};

/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "memory_trace.out", "specify trace file name");

KNOB<bool> KnobRegisterThreads(KNOB_MODE_WRITEONCE, "pintool",
    "r", "0", "threads required to be registered?");

KNOB<string> KnobFunctionsFile(KNOB_MODE_WRITEONCE, "pintool",
    "f", "trace_functions.in", "file with list of functions to trace");

KNOB<bool> KnobRequireROI(KNOB_MODE_WRITEONCE, "pintool",
    "i", "0", "require region of interest annotation?");

KNOB<int64_t> KnobNumAddressLocks (KNOB_MODE_WRITEONCE, "pintool",
    "l", "64", "number of locks for simulated cache coherence");

KNOB<int64_t> KnobBlockSize (KNOB_MODE_WRITEONCE, "pintool",
    "b", "64", "cache line/block size to simulate");

KNOB<int64_t> KnobAccessesBeforeFlush (KNOB_MODE_WRITEONCE, "pintool",
    "a", "64", "accesses per thread before flushing");

KNOB<bool> KnobTurnOff (KNOB_MODE_WRITEONCE, "pintool",
    "t", "0", "turn off address locking for test (should produce incorrect results)");

KNOB<int64_t> KnobTimestampDifference (KNOB_MODE_WRITEONCE, "pintool",
    "d", "1000", "How far 2 threads can differ in timestamp before a thread attempts to flush the other's buffer and update its timestamp");

/* ===================================================================== */
/* Helper routines                                                     */
/* ===================================================================== */

// requires thread_start_fini_lock already held
// acquires thread_start_fini_lock then all thread locks
// returns max timestamp of all threads
int64_t acquire_all_thread_locks() {
  set<int64_t>::iterator it;
  int64_t max_time = 0;
  for (it = pin_threadid_set.begin(); it != pin_threadid_set.end(); ++it) {
    thread_data_t *tdata = get_tls(*it);
    PIN_MutexLock(&tdata->thread_lock);
    if (tdata->lamport_timestamp > max_time) max_time = tdata->lamport_timestamp;
  }
  return max_time;
}

// requires thread_start_fini_lock already held
// release all locks, first setting a new timestamp
void release_all_thread_locks(int64_t new_timestamp) {
  set<int64_t>::iterator it;
  for (it = pin_threadid_set.begin(); it != pin_threadid_set.end(); ++it) {
    thread_data_t *tdata = get_tls(*it);
    tdata->lamport_timestamp = new_timestamp;
    PIN_MutexUnlock(&tdata->thread_lock);
  }
}

// given memory address and size of access, return the first lock index
// if to_lock is not null set it to the number of locks that must be acquired
int64_t lock_index(uint64_t address, uint64_t size, int64_t *to_lock) {
  uint64_t removed_blocks = address >> block_size_log;
  uint64_t end_removed_blocks = (address+size-1) >> block_size_log;
  int64_t number_to_lock = end_removed_blocks - removed_blocks + 1;
  // can lock at most num_locks
  if (number_to_lock > num_locks) number_to_lock = num_locks;
  if (to_lock) *to_lock = number_to_lock;
  return removed_blocks % num_locks;
}

// determine all unique lock_address indices that we must lock
// lock them in index order
// place these indices in index_lock_list
// NOTE: I assume that interrupts cannot occur when this is called,
// as asynchronous interrupts are delayed until the end of the trace,
// and synchronous interrupts from the instrumented program occur
// with/after that instruction.
// See: http://tech.groups.yahoo.com/group/pinheads/message/7742
//
// This allows ctxt_change handler to assume that index_lock_list
// is always consistent
//
// returns the max of the lamport timestamps on acquired locks
int64_t acquire_address_locks(THREADID pin_threadid) {
  thread_data_t* tdata = get_tls(pin_threadid);
  int64_t to_lock;
  if (tdata->is_read) {
    int64_t index = lock_index(tdata->read1_address, tdata->read_size, &to_lock);
    for (int64_t i = 0; i < to_lock; ++i) {
      tdata->index_lock_list.push_back(index);
      index = (index+1)%num_locks;
    }
  }
  if (tdata->is_read2) {
    int64_t index = lock_index(tdata->read2_address, tdata->read_size, &to_lock);
    for (int64_t i = 0; i < to_lock; ++i) {
      tdata->index_lock_list.push_back(index);
      index = (index+1)%num_locks;
    }
  }
  if (tdata->is_write) {
    int64_t index = lock_index(tdata->write_address, tdata->write_size, &to_lock);
    for (int64_t i = 0; i < to_lock; ++i) {
      tdata->index_lock_list.push_back(index);
      index = (index+1)%num_locks;
    }
  }

  // at this point index_lock_list unsorted and may contain duplicates
  std::sort(tdata->index_lock_list.begin(), tdata->index_lock_list.end());
  vector<int64_t>::iterator it;
  it = std::unique(tdata->index_lock_list.begin(), tdata->index_lock_list.end());
  tdata->index_lock_list.resize(std::distance(tdata->index_lock_list.begin(), it));

  int64_t max_time = 0;
  for (it = tdata->index_lock_list.begin(); it != tdata->index_lock_list.end(); ++it) {
    PIN_MutexLock(&address_lock_bank[*it].lock);
    max_time = max_time > address_lock_bank[*it].lamport_timestamp ? max_time : address_lock_bank[*it].lamport_timestamp;
  }
  return max_time;
}

// Release all locks in index_lock_list
// set their timestamp from the thread's
// clear the list
void release_address_locks(THREADID pin_threadid) {
  thread_data_t* tdata = get_tls(pin_threadid);
  vector<int64_t>::iterator it;
  for (it = tdata->index_lock_list.begin(); it != tdata->index_lock_list.end(); ++it) {
    int64_t idx = (*it);
    address_lock_bank[idx].lamport_timestamp = tdata->lamport_timestamp;
    PIN_MutexUnlock(&address_lock_bank[idx].lock);
  }
  tdata->index_lock_list.clear();
}

/* ===================================================================== */
/* Analysis routines                                                     */
/* these functions (in particular the memory ones) may hold address      */
/* locks beyond the duration of the call                                 */
/* ===================================================================== */

//////////////
// memory access functions
//////////////

void memory_access_header_a(THREADID pin_threadid) {
  release_address_locks(pin_threadid);
  thread_data_t* tdata = get_tls(pin_threadid);

  int64_t threadid = tdata->user_threadid;
  {
    pin_critical_section CS(&tdata->thread_lock);
    bool do_trace = in_roi || !require_roi;
    tdata->trace_this_access = do_trace && threadid >= 0;
  }
  tdata->is_read  = false;
  tdata->is_read2 = false;
  tdata->is_write = false;
}

void  memory_access_read1_a(
    THREADID pin_threadid
  , ADDRINT address
  , UINT32 size
  ) {
  thread_data_t* tdata = get_tls(pin_threadid);
  if (tdata->trace_this_access) {
    tdata->is_read = true;
    tdata->read1_address = address;
    tdata->read_size = size;
  }
}

void  memory_access_read2_a(
    THREADID pin_threadid
  , ADDRINT address
  ) {
  thread_data_t* tdata = get_tls(pin_threadid);
  if (tdata->trace_this_access) {
    tdata->is_read2 = true;
    tdata->read2_address = address;
  }
}

void  memory_access_write_a(
    THREADID pin_threadid
  , ADDRINT address
  , UINT32 size
  ) {
  thread_data_t* tdata = get_tls(pin_threadid);
  if (tdata->trace_this_access) {
    tdata->is_write = true;
    tdata->write_address = address;
    tdata->write_size = size;
  }
}

void memory_access_footer_a(THREADID pin_threadid) {
  thread_data_t* tdata = get_tls(pin_threadid);

  // acquire necessary locks (global or addresses) and trace
  if (tdata->trace_this_access) {
    int64_t threadid = tdata->user_threadid;
    // locks will be released and timestamps updated at next function
    int64_t max_locked_timestamp;
    if (turn_off_locks) {
      max_locked_timestamp = 0;
    } else {
      max_locked_timestamp = acquire_address_locks(pin_threadid);
    }

    {
      pin_critical_section CS(&tdata->thread_lock);
      if (*static_cast<volatile bool*>(&tdata->trace_this_access)) {
        int64_t new_time = max_locked_timestamp;
        if (tdata->lamport_timestamp > new_time) new_time = tdata->lamport_timestamp;
        ++new_time;

        tdata->trace_stream << new_time << '\t' << threadid << "\tm";
        if (tdata->is_read) {
          tdata->trace_stream << "\tr" <<
            "\t" << tdata->read1_address <<
            "\t" << tdata->read_size;
        }
        if (tdata->is_read2) {
          tdata->trace_stream << "\tr2" <<
            "\t" << tdata->read2_address;
        }
        if (tdata->is_write) {
          tdata->trace_stream << "\tw" <<
            "\t" << tdata->write_address <<
            "\t" << tdata->write_size;
        }
        tdata->trace_stream << "\n";
        tdata->touch_buffer(pin_threadid, new_time, false, &CS, false);
      }
    }
  }
}

// instructions that do not access memory should
// still attempt to release locks
void memory_access_release_a(THREADID pin_threadid) {
  release_address_locks(pin_threadid);
}

//////////////
// end memory access functions
//////////////


void function_call_a(
    CHAR *name
  , THREADID pin_threadid
  , ADDRINT stack_pointer
  , ADDRINT arg1
  , ADDRINT arg2
  , ADDRINT arg3
  ) {
  release_address_locks(pin_threadid);
  // always trace regardless of thread or ROI
  // (may need to trace experiment startup before ROI or registered threads)
  thread_data_t *tdata = get_tls(pin_threadid);
  int64_t threadid = tdata->user_threadid;
  bool registered = threadid >= 0;
  int64_t time = -1;

  {
    pin_critical_section CS(&thread_start_fini_lock.lock, !registered);
    if (!registered) {
      time = acquire_all_thread_locks();
    }

    {
      pin_critical_section CS(&tdata->thread_lock, registered);
      time = threadid < 0 ? time : tdata->lamport_timestamp;
      ++time;
      tdata->lamport_timestamp = time;

      tdata->trace_stream <<
        time << '\t' << threadid << "\tfc"  <<
        "\t" << name << 
        "\t" << stack_pointer <<
        "\t" << arg1 <<
        "\t" << arg2 <<
        "\t" << arg3 <<
        "\n";
      tdata->touch_buffer(pin_threadid, time, !registered, &CS, !registered);
    }

    if (!registered) {
      release_all_thread_locks(time);
    }
  }
}

void function_return_a(
    CHAR *name
  , THREADID pin_threadid
  , ADDRINT stack_pointer
  , ADDRINT return_value
  ) {
  release_address_locks(pin_threadid);
  thread_data_t *tdata = get_tls(pin_threadid);
  int64_t threadid = tdata->user_threadid;
  bool registered = threadid >= 0;
  // always trace regardless of thread or ROI
  // (may need to trace experiment startup before ROI or registered threads)
  int64_t time = -1;

  {
    pin_critical_section CS(&thread_start_fini_lock.lock, !registered);
    if (!registered) {
      time = acquire_all_thread_locks();
    }

    {
      pin_critical_section CS(&tdata->thread_lock, registered);
      time = threadid < 0 ? time : tdata->lamport_timestamp;
      ++time;

      tdata->trace_stream <<
        time << '\t' << threadid << "\tfr" <<
        "\t" << name <<
        "\t" << stack_pointer <<
        "\t" << return_value <<
        "\n";
      tdata->touch_buffer(pin_threadid, time, !registered, &CS, !registered);
    }

    if (!registered) {
      release_all_thread_locks(time);
    }
  }
}

// Always synchronize threads, even if this occurs on registered thread
void change_roi_a(THREADID pin_threadid, bool new_roi, CHAR *change_to) {
  release_address_locks(pin_threadid);
  thread_data_t *tdata = get_tls(pin_threadid);

  {
    pin_critical_section CS(&thread_start_fini_lock.lock);
    int64_t time = acquire_all_thread_locks() + 1;
    in_roi = new_roi;
    tdata->trace_stream << time << '\t' << tdata->user_threadid <<
      '\t' << change_to << "_roi\n";
    tdata->touch_buffer(pin_threadid, time, true, NULL, true);
    release_all_thread_locks(time);
  }
}

/* ===================================================================== */
/* Callback routines                                                     */
/* these may not hold locks beyond duration of call                      */
/* ===================================================================== */

//////////////
// thread start and end tracing
//
// for consistent merging must hold all thread locks while forcing
// thread registration to global file
//////////////

// helper
// must hold thread_start_fini_lock and all thread locks
void trace_start_thread(THREADID pin_threadid, int64_t time) {
  thread_data_t *tdata = get_tls(pin_threadid);
  tdata->trace_stream << time << '\t' << tdata->user_threadid << "\ttr\n";
  tdata->touch_buffer(pin_threadid, time, true, NULL, true); // force flush for merging
}

void  register_thread_a(THREADID pin_threadid, ADDRINT user_threadid) {
  release_address_locks(pin_threadid);
  thread_data_t *tdata = get_tls(pin_threadid);
  if (register_threads) {
    pin_critical_section CS(&thread_start_fini_lock.lock);
    int64_t new_time = acquire_all_thread_locks() + 1;

    tdata->user_threadid = user_threadid;
    trace_start_thread(pin_threadid, new_time);

    release_all_thread_locks(new_time);
  }
}

void thread_start_a(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v) {
  thread_data_t *tdata = new thread_data_t();
  {
    pin_critical_section CS(&thread_start_fini_lock.lock);
    ++num_threads;
    PIN_SetThreadData(tls_key, tdata, threadid);
    
    if (!register_threads) {
      tdata->user_threadid = PIN_ThreadUid();
      // synchronize threads -- already holding thread_s/f lock
      int64_t start_timestamp = acquire_all_thread_locks() + 1;
      trace_start_thread(threadid, start_timestamp);
      release_all_thread_locks(start_timestamp);
    } else {
      pin_critical_section CS2(&tdata->thread_lock);
      tdata->lamport_timestamp = 0;
    }

    pin_threadid_set.insert(threadid);
    {
      pin_critical_section CS3(&file_lock.lock);
      last_flushed.left.insert(std::pair<int64_t, int64_t>(threadid, 0));
    }
  }
}

void thread_fini_a(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v) {
  release_address_locks(threadid);
  thread_data_t *tdata = get_tls(threadid);
  bool registered_thread = tdata->user_threadid >= 0;
  {
    pin_critical_section CS(&thread_start_fini_lock.lock);
    if (registered_thread) {
      // synchronize threads -- already holding thread_s/f lock
      int64_t timestamp = acquire_all_thread_locks() + 1;
      tdata->trace_stream << timestamp << '\t' << tdata->user_threadid << "\ttf\n";
      tdata->touch_buffer(threadid, timestamp, true, NULL, true); // force flush for merging
      release_all_thread_locks(timestamp);
    }

    pin_threadid_set.erase(threadid);
    // always flush -- might be an unregistered thread with function traces
    {
      pin_critical_section CS2(&tdata->thread_lock);
      if (tdata->buffered_entries > 0) {
        pin_critical_section CS3(&file_lock.lock);
        trace_file << tdata->trace_stream.rdbuf();
        assert(trace_file);
        tdata->trace_stream.str(std::string());
        last_flushed.left.erase(threadid);
      }
    }
    delete get_tls(threadid);
  }
}

//////////////
// end thread start and end tracing
//////////////

void fini_a(INT32 code, VOID *v) {
  // We do not have access to a THREADID, leading me to believe this callback
  // occurs only after all threads (including main thread) have joined
  // no need to release locks
  {
    pin_critical_section CS(&file_lock.lock);
    trace_file.close();
  }
}

// only record if on a registered thread
void ctxt_change_release(
    THREADID pin_threadid 
  , CONTEXT_CHANGE_REASON reason
  , const CONTEXT *from
  , CONTEXT *to
  , INT32 info
  , VOID *v
  ) {
  release_address_locks(pin_threadid);
  thread_data_t *tdata = get_tls(pin_threadid);
  int64_t threadid = tdata->user_threadid;
  {
    pin_critical_section CS(&tdata->thread_lock);
    int64_t time = tdata->lamport_timestamp + 1;
    tdata->trace_stream
      << time << '\t' << threadid << "\tctxt_change\n";
    tdata->touch_buffer(pin_threadid, time, false, &CS, false);
  }
}

/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID Image(IMG img, VOID *v)
{
    vector<string>* trace_functions = reinterpret_cast< vector<string>* >(v);
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
      for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn)) {
      string und_func_name = PIN_UndecorateSymbolName(RTN_Name(rtn), UNDECORATION_NAME_ONLY);

      if (und_func_name == "atomic_trace::register_thread") {
        RTN_Open(rtn);
        RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)register_thread_a,
          IARG_THREAD_ID,
          IARG_FUNCARG_ENTRYPOINT_VALUE, 0, // arg1 -- user threadid
          IARG_END);
        RTN_Close(rtn);
      }
      if (und_func_name == "atomic_trace::start_roi") {
        RTN_Open(rtn);
        RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)change_roi_a
          , IARG_THREAD_ID
          , IARG_BOOL, true
          , IARG_ADDRINT, "start"
          , IARG_END);
        RTN_Close(rtn);
      }
      if (und_func_name == "atomic_trace::end_roi") {
        RTN_Open(rtn);
        RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)change_roi_a
          , IARG_THREAD_ID
          , IARG_BOOL, false
          , IARG_ADDRINT, "end"
          , IARG_END);
        RTN_Close(rtn);
      }

      // try to find this function in our list
      vector<string>::iterator it = find(trace_functions->begin(), trace_functions->end(), und_func_name);
      if (it != trace_functions->end()) {
        const char *func_name = it->c_str();
        RTN_Open(rtn);
        // call traces name, new stack pointer (after call), values of first three arguments
        RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)function_call_a,
          IARG_ADDRINT, func_name,
          IARG_THREAD_ID,
          IARG_REG_VALUE, REG_STACK_PTR,
          IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
          IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
          IARG_FUNCARG_ENTRYPOINT_VALUE, 2,
          IARG_END);

        // return traces name, old stack pointer (before return), value of return
        RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)function_return_a,
          IARG_ADDRINT, func_name,
          IARG_THREAD_ID,
          IARG_REG_VALUE, REG_STACK_PTR,
          IARG_FUNCRET_EXITPOINT_VALUE,
          IARG_END);
        RTN_Close(rtn);
      }
    }
  }
}

VOID Instruction(INS ins, void * v) {
  if (INS_IsMemoryRead(ins) || INS_IsMemoryWrite(ins)) {
    INS_InsertPredicatedCall(
      ins, IPOINT_BEFORE, (AFUNPTR) memory_access_header_a
      , IARG_THREAD_ID
      , IARG_END
      );

    if (INS_IsMemoryRead(ins)) {
      INS_InsertPredicatedCall(
        ins, IPOINT_BEFORE, (AFUNPTR) memory_access_read1_a
        , IARG_THREAD_ID
        , IARG_MEMORYREAD_EA
        , IARG_MEMORYREAD_SIZE
        , IARG_END
        );
    }

    if (INS_HasMemoryRead2(ins)) {
      INS_InsertPredicatedCall(
        ins, IPOINT_BEFORE, (AFUNPTR) memory_access_read2_a
        , IARG_THREAD_ID
        , IARG_MEMORYREAD2_EA
        , IARG_END
        );
    }

    if (INS_IsMemoryWrite(ins)) {
      INS_InsertPredicatedCall(
        ins, IPOINT_BEFORE, (AFUNPTR) memory_access_write_a
        , IARG_THREAD_ID
        , IARG_MEMORYWRITE_EA
        , IARG_MEMORYWRITE_SIZE
        , IARG_END
        );
    }

    INS_InsertPredicatedCall(
      ins, IPOINT_BEFORE, (AFUNPTR) memory_access_footer_a
      , IARG_THREAD_ID
      , IARG_END
      );
  } else {
    INS_InsertPredicatedCall(
      ins, IPOINT_BEFORE, (AFUNPTR) memory_access_release_a
      , IARG_THREAD_ID
      , IARG_END
      );
  }
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
   
INT32 Usage()
{
    cerr << "This tool produces a consistent memory access trace and persistence annotation." << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

/* ===================================================================== */
/* Helper functions for Main                                             */
/* ===================================================================== */

vector<string>* read_trace_functions(string file_name) {
  vector<string> *vec = new vector<string>;

  std::ifstream function_file(file_name.c_str());
  string func;
  while (function_file.good()) {
    getline(function_file, func);
    boost::algorithm::trim(func);
    vec->push_back(func);
  }

  return vec;
}

bool is_power_2(int64_t num) {
  return ((num > 0) && !(num & (num - 1)));
}

int64_t binary_log(int64_t num) {
  if (num <= 0) return 0;
  uint64_t unum = num;
  int64_t l = 0;
  while (unum >>= 1) ++l;
  return l;
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    // Initialize pin & symbol manager
    PIN_InitSymbols();
    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }

    register_threads = KnobRegisterThreads.Value();
    require_roi = KnobRequireROI.Value();
    vector<string>* trace_functions = read_trace_functions(KnobFunctionsFile.Value());

    accesses_flush = KnobAccessesBeforeFlush.Value();
    
    // these must both be a power of 2
    num_locks = KnobNumAddressLocks.Value();
    assert(is_power_2(num_locks));
    block_size = KnobBlockSize.Value();
    assert(is_power_2(block_size));
    block_size_log = binary_log(block_size);
    turn_off_locks = KnobTurnOff.Value();
    timestamp_difference = KnobTimestampDifference.Value();

    address_lock_bank = new address_lock_t [num_locks];
    for (int64_t i = 0; i < num_locks; ++i) {
      address_lock_bank[i].lamport_timestamp = 0;
      PIN_MutexInit(&address_lock_bank[i].lock);
    }
    tls_key = PIN_CreateThreadDataKey(0);
    PIN_MutexInit(&file_lock.lock);
    PIN_MutexInit(&thread_start_fini_lock.lock);

    last_flushed_cache = 0;

    // Write to a file since cout and cerr maybe closed by the application
    trace_file.open(KnobOutputFile.Value().c_str());
    trace_file << dec;
    trace_file.setf(ios::showbase);
    
    // Register Image to be called to instrument functions.
    IMG_AddInstrumentFunction(Image, trace_functions);
    INS_AddInstrumentFunction(Instruction, 0);

    PIN_AddThreadStartFunction(thread_start_a, 0);
    PIN_AddThreadFiniFunction(thread_fini_a, 0);
    PIN_AddFiniFunction(fini_a, 0);
    PIN_AddContextChangeFunction(ctxt_change_release, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
}

