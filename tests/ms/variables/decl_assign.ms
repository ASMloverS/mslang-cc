value := 1
value = 2
assert(value == 2)
{
    value := value + 1
    assert(value == 3)
}
assert(value == 2)
len := 5
assert(len == 5)
print("decl_assign ok")
