#!/bin/bash
sort -k 1 -n memory_trace.out | sed '/thread_sync/d'
