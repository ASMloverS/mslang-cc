#include "object/ms_operator.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_memory.h"
#include "object/ms_string.h"
#include "vm/ms_state_internal.h"

static void msObjectSetOom(MsState* L) {
  L->error = NULL;
  L->errorResult = MS_ERROR_OOM;
}

static struct MsObject* msObjectIntegerOverflow(MsState* L) {
  msRaiseRuntimeError(L, "integer overflow");
  return NULL;
}

static struct MsObject* msObjectDivisionByZero(MsState* L) {
  msRaiseRuntimeError(L, "division by zero");
  return NULL;
}

static struct MsObject* msObjectBinaryTypeError(MsState* L) {
  msRaiseTypeError(L, "unsupported binary operands");
  return NULL;
}

static struct MsObject* msObjectUnaryTypeError(MsState* L) {
  msRaiseTypeError(L, "unsupported unary operand");
  return NULL;
}

static bool msObjectIsNumeric(const struct MsObject* value) {
  MsTypeTag tag = msObjectTag(value);
  return tag == MS_TYPE_INT || tag == MS_TYPE_FLOAT;
}

static double msObjectNumericValue(const struct MsObject* value) {
  return msObjectTag(value) == MS_TYPE_INT
      ? (double)msIntWordValue(value)
      : msFloatValue(value);
}

static int msStringCompare(const struct MsString* lhs, const struct MsString* rhs) {
  size_t lhsLen = msStringLen(lhs);
  size_t rhsLen = msStringLen(rhs);
  size_t commonLen = lhsLen < rhsLen ? lhsLen : rhsLen;
  int compared = commonLen == 0 ? 0 : memcmp(msStringData(lhs), msStringData(rhs), commonLen);
  if (compared != 0) {
    return compared;
  }
  if (lhsLen < rhsLen) {
    return -1;
  }
  if (lhsLen > rhsLen) {
    return 1;
  }
  return 0;
}

static struct MsObject* msObjectStringBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs,
    struct MsObject* rhs) {
  const struct MsString* lhsString = (const struct MsString*)lhs;
  const struct MsString* rhsString = (const struct MsString*)rhs;
  if (op == MS_OP_ADD) {
    size_t lhsLen = msStringLen(lhsString);
    size_t rhsLen = msStringLen(rhsString);
    if (lhsLen > SIZE_MAX - rhsLen || lhsLen + rhsLen == SIZE_MAX) {
      msObjectSetOom(L);
      return NULL;
    }
    size_t len = lhsLen + rhsLen;
    char* buffer = msAlloc(len + 1);
    if (buffer == NULL) {
      msObjectSetOom(L);
      return NULL;
    }
    if (lhsLen > 0) {
      memcpy(buffer, msStringData(lhsString), lhsLen);
    }
    if (rhsLen > 0) {
      memcpy(buffer + lhsLen, msStringData(rhsString), rhsLen);
    }
    struct MsString* result = msStringNew(&L->heap, buffer, len);
    msFree(buffer);
    if (result == NULL) {
      msObjectSetOom(L);
      return NULL;
    }
    return (struct MsObject*)result;
  }

  int compared = msStringCompare(lhsString, rhsString);
  switch (op) {
    case MS_OP_EQ:
      return msNewBool(L, compared == 0);
    case MS_OP_NE:
      return msNewBool(L, compared != 0);
    case MS_OP_LT:
      return msNewBool(L, compared < 0);
    case MS_OP_LE:
      return msNewBool(L, compared <= 0);
    case MS_OP_GT:
      return msNewBool(L, compared > 0);
    case MS_OP_GE:
      return msNewBool(L, compared >= 0);
    default:
      return msObjectBinaryTypeError(L);
  }
}

static struct MsObject* msObjectIntBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs,
    struct MsObject* rhs) {
  int64_t lhsValue = msIntWordValue(lhs);
  int64_t rhsValue = msIntWordValue(rhs);
  switch (op) {
    case MS_OP_ADD:
    case MS_OP_SUB:
    case MS_OP_MUL:
    case MS_OP_POW:
    case MS_OP_SHL:
      return msObjectIntegerOverflow(L);
    case MS_OP_DIV:
      if (rhsValue == 0) {
        return msObjectDivisionByZero(L);
      }
      return msNewFloat(L, (double)lhsValue / (double)rhsValue);
    case MS_OP_FLOORDIV:
    case MS_OP_MOD:
      if (rhsValue == 0) {
        return msObjectDivisionByZero(L);
      }
      return msObjectIntegerOverflow(L);
    case MS_OP_BITAND:
      return msNewInt(L, lhsValue & rhsValue);
    case MS_OP_BITOR:
      return msNewInt(L, lhsValue | rhsValue);
    case MS_OP_BITXOR:
      return msNewInt(L, lhsValue ^ rhsValue);
    case MS_OP_SHR: {
      if (rhsValue < 0 || rhsValue >= 64) {
        msRaiseRuntimeError(L, "invalid shift count");
        return NULL;
      }
      unsigned int count = (unsigned int)rhsValue;
      int64_t shifted = lhsValue >= 0
          ? (int64_t)((uint64_t)lhsValue >> count)
          : -1 - (int64_t)((~(uint64_t)lhsValue) >> count);
      return msNewInt(L, shifted);
    }
    default:
      return msObjectBinaryTypeError(L);
  }
}

static struct MsObject* msObjectNumericBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs,
    struct MsObject* rhs) {
  double lhsValue = msObjectNumericValue(lhs);
  double rhsValue = msObjectNumericValue(rhs);
  double result;
  switch (op) {
    case MS_OP_ADD:
      result = lhsValue + rhsValue;
      break;
    case MS_OP_SUB:
      result = lhsValue - rhsValue;
      break;
    case MS_OP_MUL:
      result = lhsValue * rhsValue;
      break;
    case MS_OP_DIV:
      if (rhsValue == 0.0) {
        return msObjectDivisionByZero(L);
      }
      result = lhsValue / rhsValue;
      break;
    case MS_OP_FLOORDIV:
      if (rhsValue == 0.0) {
        return msObjectDivisionByZero(L);
      }
      result = floor(lhsValue / rhsValue);
      break;
    case MS_OP_MOD:
      if (rhsValue == 0.0) {
        return msObjectDivisionByZero(L);
      }
      result = fmod(lhsValue, rhsValue);
      if (result == 0.0) {
        result = copysign(0.0, rhsValue);
      } else if ((result < 0.0) != (rhsValue < 0.0)) {
        result += rhsValue;
      }
      break;
    case MS_OP_POW:
      result = pow(lhsValue, rhsValue);
      break;
    default:
      return msObjectBinaryTypeError(L);
  }
  return msNewFloat(L, result);
}

struct MsObject* msObjectBinaryOp(MsState* L, MsOpCode op, struct MsObject* lhs, struct MsObject* rhs) {
  if (lhs == NULL || rhs == NULL) {
    return msObjectBinaryTypeError(L);
  }
  if (msObjectTag(lhs) == MS_TYPE_STRING && msObjectTag(rhs) == MS_TYPE_STRING) {
    return msObjectStringBinaryOp(L, op, lhs, rhs);
  }
  if (op == MS_OP_EQ || op == MS_OP_NE) {
    bool equal = msObjectValueEquals(lhs, rhs);
    return msNewBool(L, op == MS_OP_EQ ? equal : !equal);
  }
  if (msObjectTag(lhs) == MS_TYPE_INT && msObjectTag(rhs) == MS_TYPE_INT) {
    return msObjectIntBinaryOp(L, op, lhs, rhs);
  }
  if (msObjectIsNumeric(lhs) && msObjectIsNumeric(rhs)) {
    return msObjectNumericBinaryOp(L, op, lhs, rhs);
  }
  return msObjectBinaryTypeError(L);
}

struct MsObject* msObjectUnaryOp(MsState* L, MsOpCode op, struct MsObject* value) {
  if (value == NULL) {
    return msObjectUnaryTypeError(L);
  }
  if (op == MS_OP_NOT) {
    return msNewBool(L, !msObjectIsTruthy(value));
  }
  if (msObjectTag(value) == MS_TYPE_FLOAT && op == MS_OP_NEG) {
    return msNewFloat(L, -msFloatValue(value));
  }
  if (msObjectTag(value) == MS_TYPE_INT) {
    if (op == MS_OP_NEG) {
      return msObjectIntegerOverflow(L);
    }
    if (op == MS_OP_INVERT) {
      return msNewInt(L, ~msIntWordValue(value));
    }
  }
  return msObjectUnaryTypeError(L);
}
