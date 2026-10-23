/* Z1-F11: bridge for musl-wasm32 crt1.o → blink main() */
extern int main(int argc, char **argv, char **envp);
int __main_argc_argv_envp(int argc, char **argv, char **envp) {
  return main(argc, argv, envp);
}
