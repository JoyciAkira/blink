/* wasm-fork-stubs.c — no-op stubs for __wasm_blink_fork_* host imports.
 * These symbols are declared extern in blink/wasm-fork.c and resolved at
 * runtime by the KWA-v2 host broker. For the wasm32 link to succeed they
 * must exist as defined symbols; the host overrides them via import binding.
 */
void __wasm_blink_fork_prepare(void) {}
void __wasm_blink_fork_parent(void) {}
void __wasm_blink_fork_child(void) {}