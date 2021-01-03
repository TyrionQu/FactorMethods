from __future__ import print_function
import array
import sys, getopt

index_num = 20
line=10
n=31571
n=2623297
#n=32881
#n=1522605027922533360535618378132637429718068114961380688657908494580122963258952897654000350692006139
#n=2654580613

def calculate(bexp):
  arr = [0] * n
  index_arr = [0]*n

  space=" "
  print(space*8,end="")
  for i in range(0, line):
    index = i
    print("{: 8d}".format(index), end="")

  base=137
#  base=65537
  result=1
  for i in range(0, n + line - 1):
    index = int(i / line)
    remainder = i % line
    if (remainder == 0):
        print("\n{: 7d}:".format(index), end="")

    if (bexp == True):
      cong = (result) % n
      result = cong * base
    else:
      cong = (i ** 2) % n
    arr[cong] = arr[cong] + 1
    print("{: 8d}".format(cong), end="")

  print("\n\n", space*7,end="")
  for i in range(0, line):
    index = i
    print("{: 8d}".format(index), end="")

  for i in range(0, n):
    index = int(i / line)
    if (i % line == 0):
      print("\n{: 7d}:".format(index), end="")
    print("{: 8d}".format(arr[i]), end="")
    index_arr[arr[i]] = index_arr[arr[i]] + 1

  print("\n ")

  for i in range(0, index_num):
    print("How many numbers duplicate ",i, " times:", index_arr[i], end="\n")

if __name__ == "__main__":
  calculate(False)
  calculate(True)
