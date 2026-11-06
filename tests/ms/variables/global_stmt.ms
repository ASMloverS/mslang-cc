global value
value = 5
global first, second
first = 1
second = 2
global declared
declared := 3
assert(declared == 3)
global fifth, sixth
fifth = 5
sixth = 6
assert(value == 5)
assert(first + second + fifth + sixth == 14)
print("global_stmt ok")
