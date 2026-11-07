isTrue := true
isFalse := false
zero := 0
truthyText := "yes"
first := 1 if isTrue else 2
second := 1 if isFalse else 2
third := 1 if zero else 2
fourth := 1 if truthyText else 2
assert(first == 1)
assert(second == 2)
assert(third == 2)
assert(fourth == 1)
nested := 1 if true else 2 if false else 3
assert(nested == 1)
precedence := 2 * 3 if false else 4
assert(precedence == 4)
assert((1 if true else 2) + 3 == 4)
assert((1 if false else 2) < 3)
print("cond expr ok")
