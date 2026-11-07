sum := 0
for i := 0; i < 10; i++ {
    sum += i
}
assert(sum == 45)
count := 3
for i := 3; i > 0; i-- {
    count -= 1
}
assert(count == 0)
stepped := 0
for i := 0; i < 6; i += 2 {
    stepped += 1
}
assert(stepped == 3)
for i := 0; i < 0; i++ {
    assert(false)
}
for i := 0; i < 1; i++ {
    pass
}
oddSum := 0
for i := 0; i < 10; i++ {
    if i % 2 == 0 {
        continue
    }
    oddSum += i
}
assert(oddSum == 25)
escaped := 0
for i := 0; i < 10; i++ {
    if i == 4 {
        break
    }
    escaped += 1
}
assert(escaped == 4)
print("for clause ok")
