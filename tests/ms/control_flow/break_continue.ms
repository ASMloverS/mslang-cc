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
continueIndex := 0
continueBodies := 0
continueFallthrough := 0
for continueIndex = 0; continueIndex < 4; continueIndex += 1 {
    continueBodies += 1
    if continueIndex < 3 {
        continue
    }
    continueFallthrough += 1
}
assert(continueIndex == 4)
assert(continueBodies == 4)
assert(continueFallthrough == 1)
breakIndex := 0
breakBodies := 0
for breakIndex = 0; breakIndex < 10; breakIndex += 1 {
    breakBodies += 1
    if breakIndex == 2 {
        break
    }
    breakBodies += 10
}
assert(breakIndex == 2)
assert(breakBodies == 23)
clauseOuter := 0
clauseInnerRuns := 0
clauseIndex := 0
for clauseIndex = 0; clauseIndex < 3; clauseIndex += 1 {
    clauseOuter += 1
    innerWhile := 0
    while true {
        innerWhile += 1
        if innerWhile == 2 {
            continue
        }
        if innerWhile == 3 {
            break
        }
        clauseInnerRuns += 1
    }
    clauseOuter += 10
}
assert(clauseIndex == 3)
assert(clauseOuter == 33)
assert(clauseInnerRuns == 3)
whileOuter := 0
whileIndex := 0
forInnerRuns := 0
while whileIndex < 3 {
    whileIndex += 1
    innerFor := 0
    for innerFor = 0; innerFor < 4; innerFor += 1 {
        if innerFor == 1 {
            continue
        }
        if innerFor == 3 {
            break
        }
        forInnerRuns += 1
    }
    whileOuter += 1
}
assert(whileIndex == 3)
assert(whileOuter == 3)
assert(forInnerRuns == 6)
print("break continue ok")
