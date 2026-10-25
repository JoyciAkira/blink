/* Minimal wasm EH tag definitions required by musl wasm32 SjLj ABI.
   Only provides __c_longjmp tag; setjmp/longjmp funcs come from musl libc. */
__asm__(".globl __c_longjmp\n"
        ".tagtype __c_longjmp i32\n"
        "__c_longjmp:\n");