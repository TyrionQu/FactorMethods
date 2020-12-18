from __future__ import print_function
import array
arr = array.array('i',[0])
index_arr = array.array('i',[0, 0, 0, 0, 0, 0])

line=10
n=31571
#n=32881
#n=91
#n=352426590011477

for i in range(n):
  arr.append(0)

print("     ",end="")
for i in range(0, line):
  index = i
  if (index > 9):
    print("    ", index, end="")
  else:
    print("     ",index, end="")

#print("\n   ", i / line, end="   ")

for i in range(0, n + line - 1):
  index = i / line
  if (i % line == 0):
    if (index > 999):
      print("\n",    index, end=" ")
    elif (index > 99):
      print("\n ",   index, end=" ")
    elif (index > 9):
      print("\n  ",  index, end=" ")
    else:
      print("\n   ", index, end=" ")
  cong = (i ** 2) % n
  arr[cong] = arr[cong] + 1
  if (cong > 9999):
    print("",     cong, end=" ")
  elif (cong > 999):
    print(" ",    cong, end=" ")
  elif (cong > 99):
    print("  ",   cong, end=" ")
  elif (cong > 9):
    print("   ",  cong, end=" ")
  else:
    print("    ", cong, end=" ")

print("\n");

print("     ",end="")
for i in range(0, line):
  index = i
  if (index > 9):
    print("    ", index, end="")
  else:
    print("     ",index, end="")

for i in range(0, n):
  index = i / line
  if (index == n / 2 / 10):
    if (i % 10 == 0):
      print("\nHow many numbers duplicate 0 times:", index_arr[0], end="\n")
      print("How many numbers duplicate 2 times:", index_arr[2], end="\n")
      print("How many numbers duplicate 4 times:", index_arr[4], end="\n")
  if (i % line == 0):
    if (index > 999):
      print("\n",    index, end=" ")
    elif (index > 99):
      print("\n ",   index, end=" ")
    elif (index > 9):
      print("\n  ",  index, end=" ")
    else:
      print("\n   ", index, end=" ")
  print("    ", arr[i], end=" ")
  index_arr[arr[i]] = index_arr[arr[i]] + 1

print("\n ")

print("How many numbers duplicate 0 times:", index_arr[0], end="\n")
print("How many numbers duplicate 2 times:", index_arr[2], end="\n")
print("How many numbers duplicate 4 times:", index_arr[4], end="\n")
print("How many numbers duplicate 5 times:", index_arr[5], end="\n")
