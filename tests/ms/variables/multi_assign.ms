first, second := 1, 2
assert(first == 1)
assert(second == 2)
first, fourth := 4, 4
assert(first == 4)
assert(fourth == 4)
first, second = second, first
assert(first == 2)
assert(second == 4)
third := 3
first, second, third = second, third, first
assert(first == 4)
assert(second == 3)
assert(third == 2)
left := 1
right := 2
left, right = left + right, left - right
assert(left == 3)
assert(right == -1)
print("multi_assign ok")
