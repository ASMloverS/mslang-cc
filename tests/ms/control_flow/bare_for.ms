count := 0
for {
    count += 1
    if count == 3 {
        break
    }
}
assert(count == 3)
visited := 0
skipped := 0
for {
    visited += 1
    if visited < 4 {
        continue
    }
    skipped += 1
    break
}
assert(visited == 4)
assert(skipped == 1)
print("bare for ok")
