#include "parser/ms_ast.h"

#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

void msAstArenaInit(struct MsAstArena* arena) {
  arena->head = NULL;
  arena->totalAllocated = 0;
}

void msAstArenaDestroy(struct MsAstArena* arena) {
  struct MsAstArenaBlock* block = arena->head;
  while (block != NULL) {
    struct MsAstArenaBlock* next = block->next;
    msFree(block);
    block = next;
  }
  arena->head = NULL;
  arena->totalAllocated = 0;
}

void* msAstArenaAlloc(struct MsAstArena* arena, size_t size) {
  MS_ASSERT(size > 0);
  size_t aligned = (size + (MS_AST_ARENA_ALIGN - 1)) & ~(size_t)(MS_AST_ARENA_ALIGN - 1);
  struct MsAstArenaBlock* block = arena->head;
  if (block == NULL || block->capacity - block->used < aligned) {
    size_t capacity = aligned > MS_AST_ARENA_BLOCK_SIZE ? aligned : MS_AST_ARENA_BLOCK_SIZE;
    block = msAlloc(sizeof(struct MsAstArenaBlock) + capacity);
    if (block == NULL) {
      return NULL;
    }
    block->next = arena->head;
    block->used = 0;
    block->capacity = capacity;
    arena->head = block;
    arena->totalAllocated += capacity;
  }
  void* result = block->data + block->used;
  block->used += aligned;
  return result;
}

struct MsAst* msAstNew(struct MsAstArena* arena, MsAstKind kind, uint32_t line, uint32_t column) {
  struct MsAst* node = msAstArenaAlloc(arena, sizeof(struct MsAst));
  if (node == NULL) {
    return NULL;
  }
  memset(node, 0, sizeof(*node));
  node->kind = kind;
  node->line = line;
  node->column = column;
  return node;
}

bool msAstListPush(struct MsAstArena* arena, struct MsAstList* list, struct MsAst* node) {
  if (list->count == list->capacity) {
    size_t newCapacity = list->capacity != 0 ? list->capacity * 2 : 4;
    struct MsAst** items = msAstArenaAlloc(arena, newCapacity * sizeof(*items));
    if (items == NULL) {
      return false;
    }
    if (list->items != NULL) {
      memcpy(items, list->items, list->count * sizeof(*items));
    }
    list->items = items;
    list->capacity = newCapacity;
  }
  list->items[list->count] = node;
  ++list->count;
  return true;
}

const char* msAstKindName(MsAstKind kind) {
  static const char* const kKindNames[] = {
      "MS_AST_INVALID",
      "MS_AST_INT",
      "MS_AST_FLOAT",
      "MS_AST_STRING",
      "MS_AST_RAW_STRING",
      "MS_AST_BYTES",
      "MS_AST_BOOL",
      "MS_AST_NIL",
      "MS_AST_IDENT",
      "MS_AST_FSTRING",
      "MS_AST_FSTRING_TEXT",
      "MS_AST_FSTRING_EXPR",
      "MS_AST_LIST_LIT",
      "MS_AST_DICT_LIT",
      "MS_AST_SET_LIT",
      "MS_AST_LIST_COMP",
      "MS_AST_DICT_COMP",
      "MS_AST_SET_COMP",
      "MS_AST_UNARY",
      "MS_AST_BINARY",
      "MS_AST_COMPARE_CHAIN",
      "MS_AST_LOGICAL",
      "MS_AST_CONDITIONAL",
      "MS_AST_AWAIT",
      "MS_AST_LAMBDA",
      "MS_AST_CALL",
      "MS_AST_KW_ARG",
      "MS_AST_INDEX",
      "MS_AST_SLICE",
      "MS_AST_ATTR",
      "MS_AST_SOURCE_FILE",
      "MS_AST_BLOCK",
      "MS_AST_DECL",
      "MS_AST_ASSIGN",
      "MS_AST_AUG_ASSIGN",
      "MS_AST_EXPR_STMT",
      "MS_AST_INC_DEC",
      "MS_AST_IF",
      "MS_AST_FOR",
      "MS_AST_WHILE",
      "MS_AST_TRY",
      "MS_AST_EXCEPT_CLAUSE",
      "MS_AST_WITH",
      "MS_AST_SELECT",
      "MS_AST_SELECT_CASE",
      "MS_AST_IMPORT",
      "MS_AST_FROM_IMPORT",
      "MS_AST_RETURN",
      "MS_AST_RAISE",
      "MS_AST_DEL",
      "MS_AST_GLOBAL",
      "MS_AST_BREAK",
      "MS_AST_CONTINUE",
      "MS_AST_PASS",
      "MS_AST_FUNC_DECL",
      "MS_AST_PARAM",
      "MS_AST_CLASS_DECL",
  };
  MS_ASSERT(MS_ARRAY_LEN(kKindNames) == (size_t)MS_AST_CLASS_DECL + 1);
  MS_ASSERT(kind >= MS_AST_INVALID && kind <= MS_AST_CLASS_DECL);
  return kKindNames[kind];
}

void msAstDump(const struct MsAst* node, FILE* out) {
  // Implemented in Task 2.
  MS_UNUSED(node);
  MS_UNUSED(out);
}
