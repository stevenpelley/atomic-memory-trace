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

# framework for running atomic memory trace simulations
# takes in a trace file and a simulation object
# simulation object provides callback function and state for the sim

import re

class Trace:

  # caller is responsible for initializing sim
  def __init__(self, trace_file, sim):
    self._sim = sim
    self._trace_file = trace_file 

  def run(self):
    # need to exist before callback
    read_1_address = 0
    read_size      = 0
    read_2_address = 0
    write_address  = 0
    write_size     = 0

    for i,l in enumerate(self._trace_file):
      l = l.strip()
      l_list = l.split('\t')
      threadid = int(l_list[0])
      operation = l_list[1]

      # memory
      if operation == 'm':
        have_read_1 = len(l_list) > 2 and l_list[2] == 'r'
        have_read_2 = have_read_1 and len(l_list) > 5 and l_list[5] == 'r2'
        have_write = l_list[-3] == 'w'
        if have_read_1:
          read_1_address = int(l_list[3])
          read_size = int(l_list[4])
        if have_read_2:
          read_2_address = int(l_list[6])
        if have_write:
          write_address = int(l_list[-2])
          write_size = int(l_list[-1])
        self._sim.memory_access(
            i
          , threadid
          , have_read_1
          , have_read_2
          , have_write
          , read_1_address
          , read_size
          , read_2_address
          , write_address
          , write_size
        )

      # thread register
      elif operation == 'tr':
        self._sim.start_thread(i, threadid)

      # thread finish
      elif operation == 'tf':
        self._sim.finish_thread(i, threadid)

      # function call
      elif operation == 'fc':
        func_name = l_list[2]
        stack_pointer = int(l_list[3])
        arg1 = int(l_list[4])
        arg2 = int(l_list[5])
        arg3 = int(l_list[6])
        self._sim.function_call(i, func_name, threadid, stack_pointer, arg1, arg2, arg3)

      # function return
      elif operation == 'fr':
        func_name = l_list[2]
        stack_pointer = int(l_list[3])
        return_value = int(l_list[4])
        self._sim.function_return(i, func_name, threadid, stack_pointer, return_value)

      # start ROI
      elif operation == 'start_roi':
        self._sim.start_roi(i)

      # end ROI
      elif operation == 'end_roi':
        self._sim.end_roi(i)

      # change context
      elif operation == 'ctxt_change':
        self._sim.end_roi(i, threadid)

