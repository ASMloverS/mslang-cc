count := 0
while true {
    count += 1
    if count > 2 {
        if true {
            break
        }
    }
}
assert(count == 3)
outer := 0
innerRuns := 0
while outer < 3 {
    outer += 1
    inner := 0
    while inner < 4 {
        inner += 1
        if inner == 2 {
            continue
        }
        if inner == 4 {
            break
        }
        innerRuns += 1
    }
}
assert(outer == 3)
assert(innerRuns == 6)
print("break continue ok")
