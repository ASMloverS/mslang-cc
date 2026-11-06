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
isReady := true
{
    left, right := isReady and 1, isReady or 2
    assert(left == 1)
    assert(right == true)
    left, localThird := 3, 4
    assert(left == 3)
    assert(localThird == 4)
    left, right = right, left
    assert(left == true)
    assert(right == 3)
}
assert(left == 3)
assert(right == -1)
print("multi_assign ok")
