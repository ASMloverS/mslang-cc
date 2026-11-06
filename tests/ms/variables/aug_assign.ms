value := 2
value += 3
assert(value == 5)
value -= 1
assert(value == 4)
value *= 3
assert(value == 12)
value **= 2
assert(value == 144)
value %= 10
assert(value == 4)
value <<= 3
assert(value == 32)
value >>= 2
assert(value == 8)
value |= 3
assert(value == 11)
value &= 10
assert(value == 10)
value ^= 3
assert(value == 9)
value /= 2
assert(value == 4.5)
{
    value := 1
    value += 2
    assert(value == 3)
}
assert(value == 4.5)
print("aug_assign ok")
