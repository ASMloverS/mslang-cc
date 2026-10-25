#ifndef MSLANG_SRC_OBJECT_MS_OPERATOR_H_
#define MSLANG_SRC_OBJECT_MS_OPERATOR_H_

#include <mslang/state.h>

#include "object/ms_object.h"
#include "vm/ms_opcode.h"

struct MsObject* msObjectBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs, struct MsObject* rhs);
struct MsObject* msObjectUnaryOp(MsState* L, MsOpCode op, struct MsObject* value);

#endif  // MSLANG_SRC_OBJECT_MS_OPERATOR_H_
