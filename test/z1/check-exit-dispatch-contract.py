#!/usr/bin/env python3
"""Source-only guard of Z1 x86-64 exit/exit_group dispatch.

This guards a source-level contract; it does not replace an actual WASM build,
x86 guest execution or Linux kernel G6 certification.
"""
import pathlib
import re

source = pathlib.Path("blink/syscall.c").read_text(encoding="utf-8")


def require(condition, label):
    if not condition:
        raise AssertionError(label)


def func(name):
    pattern = rf"static _Noreturn void {name}\([^)]*\) \{{(.*?)\n\}}"
    found = re.search(pattern, source, flags=re.DOTALL)
    require(found is not None, f"nonreturning {name} missing")
    return found.group(1)


thread_exit = func("SysExitZ1")
group_exit = func("SysExitGroupZ1")
require(re.search(r"syscall\(93\s*/\*", thread_exit), "Z1 task exit must use kernel nr 93")
require(re.search(r"syscall\(94\s*/\*", group_exit), "Z1 group exit must use kernel nr 94")
require("SysExitGroup(" not in thread_exit, "thread exit must not promote to group exit")
require("KillOtherThreads" not in group_exit, "kernel owns group exit, not Blink per-instance host")
require("__builtin_trap()" in thread_exit and "__builtin_trap()" in group_exit,
        "unexpected returning task/group exits must fail closed")

thread_case = re.search(r"case 0x3C:(.*?)case 0xE7:", source, flags=re.DOTALL)
group_case = re.search(r"case 0xE7:(.*?)case 0x00F:", source, flags=re.DOTALL)
require(thread_case and group_case, "x86 exit cases missing")
for label, case, target, fallback in (
    ("thread", thread_case.group(1), "SysExitZ1(m, di)", "SysExit(m, di)"),
    ("group", group_case.group(1), "SysExitGroupZ1(m, di)", "SysExitGroup(m, di)"),
):
    require("#if !defined(HAVE_THREADS) && defined(Z1_THREAD_BROKER)" in case,
            f"{label} branch must be kernel-broker scoped")
    require(target in case, f"{label} broker must use {target}")
    require("#else" in case and fallback in case,
            f"{label} non-broker path must remain unchanged")

print("Z1_G6_EXIT_DISPATCH_CONTRACT PASS (source only, NOT G6 certification)")
