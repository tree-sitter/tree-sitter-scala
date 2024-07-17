#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEBUG
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...)
#endif

// Total payload size is 1024 bytes max
#define STACK_SIZE 100

typedef struct ScannerStack {
  int stack[STACK_SIZE];
  int top;
  int last_indentation_size;
  int last_newline_count;
  int last_column;
} ScannerStack;

static ScannerStack* createStack() {
  ScannerStack* ptr = (ScannerStack*) malloc(sizeof(ScannerStack));

  ptr -> top = 0;
  ptr -> last_indentation_size = -1;
  ptr -> last_newline_count = 0;
  ptr -> last_column = -1;
  memset(ptr -> stack, STACK_SIZE, (0));

  return ptr;
}

static bool isEmptyStack(ScannerStack *stack) { return stack->top == 0; }

static int peekStack(ScannerStack *stack) {
  return isEmptyStack(stack) ? -1 : stack->stack[stack->top - 1];
}

static void pushStack(ScannerStack *stack, unsigned int value) {
  stack->top++;
  stack->stack[stack->top - 1] = (int)value;
}

static int popStack(ScannerStack *stack) {
  if (isEmptyStack(stack)) {
    return -1;
  }
  int result = peekStack(stack);
  stack->top--;

  return result;
}

static void printStack(ScannerStack *stack, char *msg) {
  LOG("%s Stack[top = %d; ", msg, stack->top);
  for (int i = 0; i < stack->top; i++) {
    LOG("%d | ", stack->stack[i]);
  }
  LOG("]\n");
}

static unsigned serialiseStack(ScannerStack *stack, char *buf) {
  int elements = isEmptyStack(stack) ? 0 : stack->top;
  if (elements < 0) {
    elements = 0;
  }
  unsigned buf_alignment_pad = (sizeof(int) - ((uintptr_t)buf % sizeof(int))) % sizeof(int);
  unsigned result_length = buf_alignment_pad + (elements + 3) * sizeof(int);
  int *placement = (int *)(buf + buf_alignment_pad);
  memcpy(placement, stack->stack, elements * sizeof(int));
  placement[elements] = stack->last_indentation_size;
  placement[elements + 1] = stack->last_newline_count;
  placement[elements + 2] = stack->last_column;

  return result_length;
}

static void deserialiseStack(ScannerStack* stack, const char* buf, unsigned length) {
  if (length != 0) {
    unsigned buf_alignment_pad = (sizeof(int) - ((uintptr_t)buf % sizeof(int))) % sizeof(int);
    length -= buf_alignment_pad;
    int *intBuf = (int *)(buf + buf_alignment_pad);

    unsigned elements = length / sizeof(int) - 3;
    stack->top = (int)elements;
    memcpy(stack->stack, intBuf, elements * sizeof(int));
    stack->last_indentation_size = intBuf[elements];
    stack->last_newline_count = intBuf[elements + 1];
    stack->last_column = intBuf[elements + 2];
  }
}
