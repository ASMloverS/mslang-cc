index := 1
sum := 0
while index <= 100 {
    sum += index
    index += 1
}
assert(sum == 5050)
zero := 0
while false {
    zero += 1
}
assert(zero == 0)
outer := 0
innerTotal := 0
while outer < 3 {
    inner := 0
    while inner < 2 {
        innerTotal += 1
        inner += 1
    }
    outer += 1
}
assert(innerTotal == 6)
print("while ok")
