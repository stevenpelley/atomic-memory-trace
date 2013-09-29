#!/usr/bin/python
# compare the 2 input files
# if not identical find the first lines where they differ
# as well as the lamport timestamp

# return
# (True,) if they are equal and
#
# (
#   False,
#   f1 timestamp where first differ,
#   f2 timestamp where first differ,
#   f1 line,
#   f2 line
# )
# otherwise
def compare(f1, f2, failassert=False):
  # initialize by reading first lines
  f1_line = 1
  f2_line = 1

  f1_last_line = f1.readline()
  if f1_last_line != "":
    f1_time = get_time(f1_last_line)
  f2_last_line = f2.readline()
  if f2_last_line != "":
    f2_time = get_time(f2_last_line)

  f1_time = 0
  f2_time = 0

  while True:
    # read all lines from each file that have the next timestamp
    # check that the timestamp is the same for both files
    # and check that the set of lines with this timestamp are
    # identical (but different orders allowed)

    f1_cont = f1_last_line != ""
    f2_cont = f2_last_line != ""
    if f1_cont != f2_cont:
      fail(failassert)
      return (False, f1_time, f2_time, f1_line, f2_line,)
    elif (not f1_cont and not f2_cont):
      return (True,)
    # else both are continue and last_lines contain valid strings

    (bunch1, f1_last_line, f1_new_time, f1_line) = \
      create_bunch(f1, f1_last_line, f1_line, failassert)

    (bunch2, f2_last_line, f2_new_time, f2_line) = \
      create_bunch(f2, f2_last_line, f2_line, failassert)

    if (f1_new_time <= f1_time or
        f2_new_time <= f2_time or
        bunch1 != bunch2 or
        f1_new_time != f2_new_time
       ):
      fail(failassert)
      return (False, f1_time, f2_time, f1_line, f2_line,)
    f1_time = f1_new_time
    f2_time = f2_new_time

  assert False, "fell through without break?"

# create the next bunch
# return
#   (tuple bunch, new last_line, bunch's time, new line_num,)
#
# if last_line == "" then we have reached new line
# so return (True, (), "", 0, same line_num)
def create_bunch(f, last_line, last_line_num, failassert):
  if last_line == "":
    return ((), "", 0, last_line_num)
  bunch_time = get_time(last_line)
  
  line_num = last_line_num
  bunch = [last_line]
  while True: # do while
    line_num += 1
    last_line = f.readline()
    if last_line == "":
      break
    new_time = get_time(last_line)
    if new_time != bunch_time:
      break
    bunch.append(last_line)
  return (tuple(sorted(bunch)), last_line, bunch_time, line_num)

def fail(failassert):
  if failassert:
    assert False

def get_time(s):
  return int(s.split('\t', 1)[0])
  
if __name__=="__main__":
  import sys
  f1_name = sys.argv[1]
  f1 = open(f1_name)
  f2_name = sys.argv[2]
  f2 = open(f2_name)

  out = compare(f1, f2, True)

  f1.close()
  f2.close()

  if out[0]:
    print "equal"
    sys.exit(0)
  else:
    print "files differ:"
    print "file {}:\ttime: {}\tline {}".format(f1_name, out[1], out[3])
    print "file {}:\ttime: {}\tline {}".format(f2_name, out[3], out[4])
    sys.exit(1)
