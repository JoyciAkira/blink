/* B12 Target Program - Freestanding x86-64 ELF */
#define SYS_write 1
#define SYS_exit 60
#define SYS_getpid 39

static inline long syscall0(long n) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}
static inline long syscall1(long n, long a1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}
static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}

static void write_str(const char *s) {
    long len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int my_getenv(const char *envp[]) {
    for (int i = 0; envp[i]; i++) {
        const char *e = envp[i];
        if (e[0]=='B' && e[1]=='1' && e[2]=='2' && e[3]=='_' &&
            e[4]=='E' && e[5]=='N' && e[6]=='V' && e[7]=='=' &&
            e[8]=='p' && e[9]=='a' && e[10]=='s' && e[11]=='s' && e[12]=='\0')
            return 1;
    }
    return 0;
}

void _start(void) {
    long argc;
    char **argv;
    char **envp;
    
    __asm__ volatile (
        "mov (%%rsp), %0\n"
        "lea 8(%%rsp), %1\n"
        : "=r"(argc), "=r"(argv)
    );
    envp = argv + argc + 1;

    write_str("[B12-EXEC-ENTRY]\n");

    if (argc >= 2 && my_strcmp(argv[1], "arg1") == 0) {
        write_str("[B12-EXEC-ARGV-PASS]\n");
    } else {
        write_str("[B12-EXEC-ARGV-FAIL]\n");
    }

    if (my_getenv(envp)) {
        write_str("[B12-EXEC-ENV-PASS]\n");
    } else {
        write_str("[B12-EXEC-ENV-FAIL]\n");
    }

    long pid = syscall0(SYS_getpid);
    if (pid > 0) {
        write_str("[B12-EXEC-SYSCALL-PASS]\n");
    } else {
        write_str("[B12-EXEC-SYSCALL-FAIL]\n");
    }

    syscall1(SYS_exit, 37);
    __builtin_unreachable();
}