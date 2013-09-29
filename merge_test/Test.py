#!/usr/bin/python
# repeatedly test merge utility for given pintool command
# inputs:
#   script that produces output trace in memory_trace.out
#     for now the script must use fully qualified names of files
#   number of attempts
#
# for each output file that merge fails (either merge process fails)
# of output of merge and sort differs)
# produce a directory with the original memory_trace.out,
# sort.out, and merge.out

# return True if success, False if some failure
# if fails produce a 'fail' file with some output related to failure
#
# test file  and merge utility must be fully qualified names
def test1(test_file, sort_utility, merge_utility):
  import subprocess
  import shlex

  fout = open('command_out', 'w')
  ret = subprocess.call(shlex.split(test_file), stdout=fout, stderr=subprocess.STDOUT)
  assert ret == 0
  fout.close()

  # memory_trace.out now exists
  # first sort with linux sort utility
  fout = open('memory_trace.sort', 'w')
  ret = subprocess.call(shlex.split(sort_utility), stdout=fout)
  assert ret == 0
  fout.close()

  # now run merge, capturing stderr in a file
  fin = open('memory_trace.out')
  fout = open('memory_trace.merge', 'w')
  ferr = open('merge_err', 'w')
  ret = subprocess.call(shlex.split(merge_utility + " -t"), stdin = fin, stdout=fout, stderr=ferr)
  fin.close()
  fout.close()
  ferr.close()

  if ret != 0:
    return False

  f1 = open('memory_trace.sort')
  f2 = open('memory_trace.merge')

  eq_ret = CompareMerge.compare(f1, f2)
  f1.close()
  f2.close()
  if not eq_ret[0]:
    fout = open('merge_results')
    fout.write("files differ:\n")
    fout.write("file {}:\ttime: {}\tline {}\n".format(f1_name, out[1], out[3]))
    fout.write("file {}:\ttime: {}\tline {}\n".format(f2_name, out[3], out[4]))
    fout.close()

  return eq_ret[0]

if __name__=="__main__":
  import CompareMerge
  import sys
  import os.path
  import shutil

  num_test = int(sys.argv[1])
  test_file = os.path.abspath(sys.argv[2])
  sort_utility = os.path.abspath('my_sort.sh')
  merge_utility = os.path.abspath('../src/merge')

  if os.path.exists('compare_test'):
    shutil.rmtree('compare_test')
  os.mkdir('compare_test')
  os.chdir('compare_test')

  file_count = 1
  for i in range(num_test):
    dir_name = 'test{}'.format(file_count)
    os.mkdir(dir_name)
    os.chdir(dir_name)

    succ = test1(test_file, sort_utility, merge_utility)
    os.chdir('..')
    if not succ:
      file_count += 1
    else:
      shutil.rmtree(dir_name)
  fail_count = file_count - 1
  print "failures:\t{}".format(fail_count)

