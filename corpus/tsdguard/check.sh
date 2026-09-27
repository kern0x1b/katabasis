#!/bin/sh
# check.sh -- corpus/tsdguard: proves runtime/xl_tsd_guard.h's page-protection scheme (the
# runtime half of coordination/crutches.md's TPIDRRO_EL0 entry) on the build host. mmap, mprotect
# and SIGSEGV are POSIX: no device, no lift, no translate.sh needed -- this compiles main.c
# against the exact host architecture running the test and runs it directly.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
cc -O0 -Wall -o "$out/tsdguard" "$here/main.c"
"$out/tsdguard"
