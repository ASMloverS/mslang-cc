first := 1 if true else 2
second := 1 if false else 2
third := 1 if 0 else 2
fourth := 1 if "yes" else 2
assert(first == 1)
assert(second == 2)
assert(third == 2)
assert(fourth == 1)
nested := 1 if false else 2 if true else 3
assert(nested == 2)
assert((1 if true else 2) + 3 == 4)
assert((1 if false else 2) < 3)
print("cond expr ok")
