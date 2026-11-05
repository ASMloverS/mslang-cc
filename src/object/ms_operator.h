#ifndef MSLANG_SRC_OBJECT_MS_OPERATOR_H_
#define MSLANG_SRC_OBJECT_MS_OPERATOR_H_

#include <mslang/state.h>

#include "object/ms_object.h"
#include "vm/ms_opcode.h"

#define MS_OBJECT_COMPARE_UNORDERED 2

// Writes -1/0/1 for ordered values, or MS_OBJECT_COMPARE_UNORDERED for NaN.
// Returns false for unsupported type combinations.
bool msObjectCompare(const struct MsObject* lhs, const struct MsObject* rhs, int* order);

struct MsObject* msObjectBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs, struct MsObject* rhs);
struct MsObject* msObjectUnaryOp(MsState* L, MsOpCode op, struct MsObject* value);

#endif  // MSLANG_SRC_OBJECT_MS_OPERATOR_H_
