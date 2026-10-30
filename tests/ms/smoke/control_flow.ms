sum := 0
for i := 1; i <= 10; i += 1 {
    sum += i
}
if sum == 55 {
    assert(true)
} else {
    assert(false, "incorrect sum")
}
assert(sum == 55)
print("control flow ok")
