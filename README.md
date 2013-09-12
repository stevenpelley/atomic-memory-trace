atomic-memory-trace
===================

PIN-tool to produce multi-threaded atomic memory traces

PIN is a useful tool for instrumenting applications and easily producing memory
access traces.  However, tracing memory accesses from multiple threads suffers
from the atomic instrumentation problem -- instructions responsible for 
tracing/logging an access happen separately from that access.  Races between
threads may result in a different order being traced than actually occurs.
This tool provides atomic instrumentation by simulating cache coherence.  In  
addition, the tool will trace thread start/end, an optional region of interest,
and user-provided fuction calls and returns.

The primary alternative to this tool is architectural simulation.  Most
simulators are complicated to learn, complicated to use (getting OSes and 
workloads running properly may be difficult), and slow (most simulators are 
single threaded and cannot leverage multithreading to produce a faster
trace/simulation).

This README documents the tracing pintool and example test case.  This tool was 
developed using verion 2.12-58423 of PIN on Ubuntu 12.04.  There are no plans
to support operating systems other than Linux or systems other than x86_64.
The pintool relies on the boots libraries.  This software comes with no support
but may be useful to others.  This project uses the MIT license.

Quick Start
===========

build the pintool
Change into the trace directory

```
% make PIN_ROOT=<your pin root> trace
```

Run the tool as any other pintool:

```
% pin -t trace/obj-intel64/trace.so -- <your program>
```

By default, output appears in the file memory_trace.out.  All threads and 
memory accesses will be traced.  The output appears with one event (thread
start, function call, or memory access) per line, starting with an arbitrary
timestamp (used to merge events later.

Any easy way to produce useful output, sorting by timestamp and then stripping
timestamps away is to use:

```
% sort -k 1 -n memory_trace.out | sed '/thread_sync/d' | awk 'BEGIN {OFS="\t"}; {$1="";sub("\t\t","")}1' > memory_trace.clean
```

memory_trace.clean will contain properly ordered events and accesses and remove
sync entries

Tool Options
============

* -o  
    The output file name.  By default is 'memory_trace.out'
* -r  
    Do threads need to be registered?  If 0/false all memory accesses from all
    threads will be traced.  If 1/true only accesses from registered threads will
    be traced.  See annotation's atomic_trace::register_thread(threadid).
* -f  
    File with list of functions to trace
* -i  
    Use Region of Interest?  If ROI is used memory tracing will only occur while
    the ROI is active.  Thread start/stop tracing and function tracing will
    always occur.
* -l  
    Number of locks for simulated cache coherence.  Increasing this number will
    use more memory and may hurt cache performance but will improve concurrency.
    If contention occurs for specific address locks (i.e. cache lines) try
    increasing this.  A value of 1 serializes all memory accesses across threads.
* -b  
    Cache block size.  By default 64 bytes.
* -a  
    Accesses per thread before flushing.  Each thread keeps a local trace buffer
    that is occasionally flushed to the global file.  More accesses per thread
    ensures that threads grabbing the global lock does not become the primary
    bottleneck.
* -t  
    Test.  Turns off address locking, breaking atomicity.  Activate this flag to
    see the instrumentation atomicity problem.
* -d  
    Allowable timestamp difference.  If threads diverge in timestamps by beyond
    this limit threads will synch and flush other threads.  This makes merging
    the output significantly easier.

Output Format
=============

Each line contains one event as a tab delimited list.  Entries contain
threadids which may be -1 (if threads must be registered but a traced function
is called from an unregistered thread), assigned by the pintool if threads are
not required to be registered, or set by the registration function (described
later).  All entries start with a timestamp and threadid:

* memory: Each instruction may read two addresses and write one.  There are
possible sub-entries for each of these accesses.  The second read does not
contain a size field as it may only occur with the first read and have the same
size (that is, r2's size is the same as r's).

```
<timestamp> <thread> m [r <address> <size>] [r2 <address>] [w <address> <size>]
```

* thread registered:

```
<timestamp> threadid tr
```

* thread finished:

```
<timestamp> threadid tf
```

* function call: All requested functions are traced, even if not on a registered
thread.  The first 3 arguments of the function are traced as well as the stack
pointer to match up calls and returns

```
<timestamp> <threadid> fc <function name> <function stack pointer> <first arg> <second arg> <third arg>
```

* function return:

```
<timestamp> <threadid> fr <function name> <function stack pointer> <return value>
```

* start Region of Interest:

```
<timestamp> <threadid> start_roi
```

* end Region of Interest:

```
<timestamp> <threadid> end_roi
```

* context change: Context changes may interrupt the locking necessary to provide
atomic tracing.  On a context change consider that the next access may not be
traced atomically.  -- It is unclear how the PIN internals work and if this is
really a concern (I haven't observed any context changes yet).

```
<timestamp> <threadid> ctxt_change
```

* thread sync: When threads flush other threads to keep all threads close in
timestamps the merging process must be made aware of this.

```
<timestamp> <threadid> thread_sync
```

Function Tracing
================

In addition to memory accesses many functions are traced.  A few are specific
to this tool, but any user-provided function can be traced.  The provided
src/annotation.cpp and src/annotation.h (creates libannotation) provides
header and library for these functions.  In general it is easier to provide
these as a library to ensure they are not in-lined.

```
atomic_trace::register_thread
atomic_trace::start_roi
atomic_trace::end_roi
```

These functions allow the pintool to highlight a region of interest (memory
accesses outside of the region will not be traced) and name threads, useful to
match trace threads to user threads.

In addition, the pintool takes an "-f" argument that is a file with a list of
functions (one per line) that will be traced.  The functions should be listed
in their undecorated form (as per pin, see above for examples).

Merge Utility
=============

merge/merge.cpp provides a tool that takes as stdin a memory trace and pushes
the merged (by timestamp) trace to the output, stripping out the timestamp from
each entry as well as omitting sync entries.  While not the most efficient (the
sort utility is somewhat faster for file traces), it suffices and allows traces
to be piped while using a small memory footprint.

Merge is necessary because the sort utility can not be used in a pipe; the
entire file must first be available.  When merging thread streams no entry may
be output until certain that no older trace can appear on that thread.  The
guarantee is provided when a thread submits a newer trace (because each thread
trace is monotonic increasing), and the threads are kept close together via
sync traces.

This tool chain works as follow: create a special fifo file to connect pin to
merge

```
% mkfifo mypipe
```

Pipe this file to merge, and the merge to a simulator utility

```
% cat mypipe | ./merge | ./simulator
```

Start the pintool, directing the trace to the fifo file

```
% pin -t trace.so -o mypipe -- ./app
```

Make sure that names are fully qualified where necessary to reach pin,
trace.so, mypipe, and your app; and that any other desired trace arguments are used.

Alternatively, merge can be used to sort trace files, although the sort command
above may be faster.

Test - Atomic Increment
=======================

src/inc.cpp provides a test case for instrumentation atomicity.  Several
threads use atomic_fetchadd_long to repeatedly increment a shared counter.
Each thread counts the number of times it increments the counter and the number
of times the counter was even before the increment.

A trace simulation can reconstruct the operation of this program.  All memory
operations to the counter's address are atomic increments.  Simply observing
the order in which increments occurs allows us to reconstruct the number of
increments from each thread, as well as how many time each thread observes a
previously-even number on increment.

If the trace simulation matches the actual program, there is a good chance
things are working.  Additionally, using the "-t" option we can force the
pintool to test and disable locking.  If this breaks it we have even more
confidence that the tool works.

Run as

```
% cat mypipe | ./merge | ./inc_sim.py
```

and

```
% pin -t trace.so -o mypipe -f func_list -- ./inc <threads> <total incs>
```

verify that the output of both the simulation and the actual program are the
same.  Use the -t 1 flag for trace.so and see that the outputs differ.

Internals: Locking Protocol
===========================

Some rules need to be followed to prevent deadlock.
These rules adhere to the rules provided by the PIN manual.

Locks:
------

* Address locks  
    Responsible for providing instrumentation atomicity tracing an instruction
    will acquire whatever locks cover accessed addresses, and these locks are
    released on the next pin function (so all pin analaysis and callback
    functions first release all address locks).
* Thread start/finish lock  
    Acquired when a thread starts or finishes, or to block threads from
    starting or finishing.  Necessary for actions that need to synchronize
    timestamp across all threads.  Any thread that wishes to access another
    thread's data must hold this.
* Thread locks  
    Covers each thread's trace buffer and Lamport timestamp.  Other threads may
    have to read/update another thread's timestamp or flush its buffer.
* File lock  
    Covers the trace file handler, thread count, last_flushed (really all shard
    global objects).

Locking Rules
-------------

Function Callbacks must release all locks before returning.
Analysis functions (for instructions and routines) may hold address locks beyond return.
Address locks are released at the beginning of every analysis and callback
routine, treating each routine as the "end" of an instruction analysis routine.

Locks must always be acquired in the following order:

1.  Address locks by index order of the address_lock_bank
2.  Thread start/finish lock
3.  Thread locks by pin THREADID order
4.  The global lock

This implies, for example, that one may not acquire **any** address locks while
holding a thread lock

Internals: Lamport Clock
========================

Actual trace order is determined by a Lamport clock, and all trace entries use
a Lamport timestamp

All address locks keep a timestamp.  The time of an access = max(timestamp of
all address locks acquired, thread timestamp)+1.  All address locks and the
accessing thread must be updated to this access timestamp.

Functions on registered threads increment that thread's timestamp.  To enforce
an order of functions from two threads there must be corresponding memory
accesses (release and acquire, such as a lock)

Functions from unregistered threads, and roi traces, and the start and finish
of threads synchronize all threads.  This is done by acquiring the thread
start/finish lock (so that new threads do not appear)  and ALL thread locks,
moving all threads up to the latest global timestamp + 1.

Time separation: It's possible for 2 threads to diverge in time, requiring
trace merging to use a huge amount of memory (imagine 2 threads, one is
sleeping and the other executes continuously -- we cannot merge the running
thread's entries until we know the sleeping thread won't produce an older
timestamp -- this only happens once we observe a new, large timestamp).  Solve
this by keeping track of the minimum "last flushed" thread timestamp.  When a
thread tries to flush and sees that any thread is too old, it will try to flush
and update the timestamp for all too-old threads (those below some threshold),
bounding how far apart threads can be in the trace file.  Calculating the
minimum last flushed timestamp requires keeping a last_flushed bimap under the
global lock.  A last_flushed_cache timestamp (also covered by global file lock)
makes this more efficient.
