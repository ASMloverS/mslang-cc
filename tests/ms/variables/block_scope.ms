value := 1
if true {
    value := value + 1
    assert(value == 2)
    if true {
        value = 3
        assert(value == 3)
    }
    assert(value == 3)
}
assert(value == 1)
print("block_scope ok")
