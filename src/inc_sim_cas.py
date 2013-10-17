#!/usr/bin/python
# Copyright (c) 2013 Steven Pelley
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so, 
# subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in all 
# copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# analyse the inc trace.
# assert:
#   exactly 8 bytes of persistent memory are accessed
#   each access to this persistent memory is either a read or a read and write
#     (inc here is implemented with CAS)
#
# compute and print:
#   for each registered thread the number of writes to counter (should be number of incs)
#   for each registered thread the number of reads to counter
#   -- each inc will read once before the CAS, read as part of the successful CAS
#      and then once for every failure
#
# print out any context changes as these may cause inconsistencies

class my_sim:
  def __init__(self):
    self._special_address = None
    self._special_size = 0
    self._special_value = 0

    self._active_threads = set()
    import collections
    # threadid to increment
    self._increments = collections.defaultdict(int)
    self._reads = collections.defaultdict(int)

  def memory_access(self, line_num, threadid, have_read_1, have_read_2, have_write, read_1_address, read_size, read_2_address, write_address, write_size):
    if not self._special_address:
      return

    read1 = have_read_1 and not have_read_2 and not have_write

    RMW = (
      have_read_1 and not have_read_2 and have_write and 
      read_1_address == write_address and
      read_size == write_size
    )

    read1_special = ( have_read_1 and (
      (read_1_address < self._special_address and read_1_address + read_size >= self._special_address) or
      (read_1_address >= self._special_address and read_1_address <= self._special_address + self._special_size)
    ) )

    read_first_8 = have_read_1 and read_1_address == self._special_address and read_size == 8

    # atomic inc to special address?
    if RMW and read1_special:
      # check that we touch exactly the first 8 bytes
      assert read_first_8 
      self._increments[threadid] += 1
      self._special_value += 1
      self._reads[threadid] += 1
    elif read1 and read1_special:
      self._reads[threadid] += 1

  def start_thread(self, line_num, threadid):
    assert threadid not in self._active_threads
    self._active_threads.add(threadid)

  def finish_thread(self, line_num, threadid):
    assert threadid in self._active_threads
    self._active_threads.remove(threadid)
  
  def function_call(self, line_num, name, threadid, stack_pointer, arg1, arg2, arg3):
    if name == "atomic_trace::special_malloc":
      assert not self._special_address
      assert self._special_size == 0
      self._special_size = arg1
    elif name == "atomic_trace::special_free":
      pass
    else:
      assert False

  def function_return(self, line_num, name, threadid, stack_pointer, return_value):
    if name == "atomic_trace::special_malloc":
      assert not self._special_address
      assert self._special_size >= 8
      self._special_address = return_value
    elif name == "atomic_trace::special_free":
      pass
    else:
      assert False

  def start_roi(self, line_num):
    pass

  def end_roi(self, line_num):
    pass

  def ctxt_change(self, line_num, threadid):
    print("context change!  Line {}".format(line_num))

def main():
  import argparse
  parser = argparse.ArgumentParser(description='test inc atomic trace simulation')
  parser.add_argument('--infile', default="")
  args = parser.parse_args()

  import sys
  if len(args.infile) == 0:
    fin = sys.stdin
  else:
    fin = open(args.infile)

  import AtomicTrace

  sim = my_sim()
  trace = AtomicTrace.Trace(fin, sim)
  trace.run()

  # recreate the program output
  print("final counter value: {}".format(sim._special_value))
  for threadid in sorted(sim._increments.keys()):
    print("thread\t{}\t{}".format(threadid, sim._increments[threadid]))

  print("fails: {}".format(sum(sim._reads.values())-(2*sim._special_value)))

if __name__=="__main__":
  main()
