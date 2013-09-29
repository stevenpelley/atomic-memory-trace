#!/bin/bash
~/pin/pin -t ~/work/atomic-memory-trace/trace/obj-intel64/trace.so -f ~/work/atomic-memory-trace/src/func_list -i 1 -r 1 -- ~/work/atomic-memory-trace/src/inc 5 1000
