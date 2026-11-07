if true {
    assert(true)
} else {
    assert(false)
}
if false {
    assert(false)
} else if 0 {
    assert(false)
} else if "" {
    assert(false)
} else if nil {
    assert(false)
} else if "selected" {
    assert(true)
} else {
    assert(false)
}
if 0.0 {
    assert(false)
} else {
    assert(true)
}
if -1 {
    if 1 {
        assert(true)
    } else {
        assert(false)
    }
}
value := 1
if true {
    value := value + 1
    assert(value == 2)
}
assert(value == 1)
print("if else ok")
