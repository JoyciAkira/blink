#include <stdint.h>

struct WasmJumpBuffer {
  void *invocation;
  uint32_t label;
  struct {
    void *environment;
    int value;
  } argument;
};

void __wasm_setjmp(void *environment, uint32_t label, void *invocation) {
  struct WasmJumpBuffer *buffer = environment;
  if (!label || !invocation) __builtin_trap();
  buffer->invocation = invocation;
  buffer->label = label;
}

uint32_t __wasm_setjmp_test(void *environment, void *invocation) {
  struct WasmJumpBuffer *buffer = environment;
  if (!buffer->label || !invocation) __builtin_trap();
  return buffer->invocation == invocation ? buffer->label : 0;
}

void __wasm_longjmp(void *environment, int value) {
  struct WasmJumpBuffer *buffer = environment;
  if (!value) value = 1;
  buffer->argument.environment = environment;
  buffer->argument.value = value;
  __builtin_wasm_throw(1, &buffer->argument);
}

__asm__(".globl __c_longjmp\n"
        ".tagtype __c_longjmp i32\n"
        "__c_longjmp:\n");
