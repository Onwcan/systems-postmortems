#!/usr/bin/env bash
#
# Builds and runs every case, capturing the tool output into evidence/.
#
# The build flags are part of each demonstration, not incidental. Case 04 only
# misbehaves at -O2 and only explains itself under UBSan; cases 01 and 02 are
# invisible without ThreadSanitizer; case 05 needs optimisation on or the
# measurement is dominated by unoptimised loop overhead. So this is a script
# with per-case flags rather than one CMake target list.
#
# Usage: scripts/run-all.sh [output-dir]

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/evidence}"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

CXX="${CXX:-g++}"
STD="-std=c++20"
mkdir -p "$OUT"

pass=0
fail=0

banner() {
  echo
  echo "==============================================================="
  echo "$1"
  echo "==============================================================="
}

# compile <case> <variant> <output-name> <extra flags...>
compile() {
  local case_dir="$1" variant="$2" name="$3"
  shift 3
  # shellcheck disable=SC2086  # $STD is one word by construction
  "$CXX" $STD "$@" "$ROOT/cases/$case_dir/$variant.cpp" -o "$BUILD/$name" -lpthread
}

# capture <evidence-file> <command...>
# Records both the output and the exit status, because for several of these
# cases the exit status IS the result.
capture() {
  local evidence="$1"
  shift
  {
    echo "\$ $*"
    echo
    "$@" 2>&1
    # Read the status immediately. After any other command -- an echo included --
    # $? describes that command instead, which is a quiet way to record every
    # program as having succeeded.
    local status=$?
    echo
    echo "[exit status: $status]"
  } | scrub >"$OUT/$evidence" 2>&1
}

# Sanitizer reports quote absolute source paths, and the build tree is a mktemp
# directory. Both are noise in committed evidence, and the absolute path leaks
# the checkout location of whoever ran it. Rewrite them to what the READMEs
# quote: repository-relative paths and bare binary names.
scrub() {
  sed -e "s|$ROOT/||g" -e "s|$BUILD/||g"
}

record() {
  local verdict="$1" what="$2"
  if [ "$verdict" = "pass" ]; then
    echo "  PASS: $what"
    pass=$((pass + 1))
  else
    echo "  FAIL: $what"
    fail=$((fail + 1))
  fi
}

# present <description> <pattern> <evidence-file>
present() {
  if grep -q "$2" "$OUT/$3"; then
    record pass "$1"
  else
    record fail "$1"
  fi
}

# absent <description> <pattern> <evidence-file>
absent() {
  if grep -q "$2" "$OUT/$3"; then
    record fail "$1"
  else
    record pass "$1"
  fi
}

# ---------------------------------------------------------------------------
banner "01  Dropped counters -- a data race in a metrics counter"
compile 01-dropped-counters broken c01-broken -O2 -g -fsanitize=thread
compile 01-dropped-counters fixed c01-fixed -O2 -g -fsanitize=thread

capture "01-broken-tsan.txt" "$BUILD/c01-broken"
capture "01-fixed-tsan.txt" "$BUILD/c01-fixed"

present "TSan reports a data race in the broken version" \
  "WARNING: ThreadSanitizer: data race" 01-broken-tsan.txt
absent "TSan is silent on the fixed version" \
  "ThreadSanitizer" 01-fixed-tsan.txt

# ---------------------------------------------------------------------------
banner "02  Lock order inversion -- a deadlock that happens once a day"
compile 02-lock-order-inversion broken c02-broken -O2 -g -fsanitize=thread
compile 02-lock-order-inversion fixed c02-fixed -O2 -g -fsanitize=thread

# GCC's TSan has no deadlock detector, so the program carries its own watchdog
# and reports the wait-for cycle instead of hanging until something kills it.
# The outer timeout is a backstop, not the mechanism.
capture "02-broken-tsan.txt" timeout 30 "$BUILD/c02-broken"
capture "02-fixed-tsan.txt" timeout 30 "$BUILD/c02-fixed"

if grep -q "lock-order-inversion" "$OUT/02-broken-tsan.txt"; then
  # Only Clang's runtime does this. Kept so the check does the right thing if
  # the case is ever built with a toolchain that supports it.
  record pass "TSan reports lock-order-inversion before any hang"
elif grep -q "cycle: alice -> bob -> alice" "$OUT/02-broken-tsan.txt"; then
  record pass "the watchdog names the wait-for cycle"
elif grep -q "exit status: 124" "$OUT/02-broken-tsan.txt"; then
  record fail "the watchdog names the cycle (only a bare timeout was seen)"
else
  record fail "the broken version deadlocks and is diagnosed"
fi

present "the fixed version completes" "completed" 02-fixed-tsan.txt

# ---------------------------------------------------------------------------
banner "03  Dangling reference -- a use-after-free hiding behind a vector"
compile 03-dangling-reference broken c03-broken -O1 -g -fsanitize=address
compile 03-dangling-reference fixed c03-fixed -O1 -g -fsanitize=address

capture "03-broken-asan.txt" "$BUILD/c03-broken"
capture "03-fixed-asan.txt" "$BUILD/c03-fixed"

present "ASan reports heap-use-after-free" \
  "heap-use-after-free" 03-broken-asan.txt
absent "ASan is silent on the fixed version" \
  "AddressSanitizer" 03-fixed-asan.txt

# ---------------------------------------------------------------------------
banner "04  Overflow removes the check -- works in debug, broken in release"
# Four builds of ONE source file, because the point is that they disagree.
compile 04-overflow-removes-check broken c04-broken-O0 -O0 -g
compile 04-overflow-removes-check broken c04-broken-O2 -O2 -g
compile 04-overflow-removes-check broken c04-broken-ub -O2 -g -fsanitize=undefined
compile 04-overflow-removes-check broken c04-broken-wv -O2 -g -fwrapv
compile 04-overflow-removes-check fixed c04-fixed-O2 -O2 -g -fsanitize=undefined

capture "04-broken-O0.txt" "$BUILD/c04-broken-O0"
capture "04-broken-O2.txt" "$BUILD/c04-broken-O2"
capture "04-broken-ubsan.txt" "$BUILD/c04-broken-ub"
capture "04-broken-fwrapv.txt" "$BUILD/c04-broken-wv"
capture "04-fixed-O2.txt" "$BUILD/c04-fixed-O2"

# The strongest evidence is not the output, it is the object code: at -O2 the
# guard is not merely bypassed, it is absent.
disassemble_fits() {
  # The mangled name of an internal-linkage function in an anonymous namespace.
  # Fall back to any symbol containing "fits" so a change of compiler or ABI
  # produces a different listing rather than an empty file.
  objdump -d --no-show-raw-insn "$1" |
    sed -n '/<_ZN12_GLOBAL__N_1L4fitsEii>:/,/ret/p' |
    grep . ||
    objdump -d --no-show-raw-insn "$1" | sed -n '/fits.*>:/,/ret/p'
}

{
  echo "The overflow guard 'if (end < offset) return false;' as compiled."
  echo
  echo "=== -O0: the comparison is present ==="
  disassemble_fits "$BUILD/c04-broken-O0"
  echo
  echo "=== -O2: the comparison is gone ==="
  disassemble_fits "$BUILD/c04-broken-O2"
} | scrub >"$OUT/04-broken-disassembly.txt" 2>&1

present "at -O2 the guard is gone and the check accepts" \
  "ACCEPTED, this is the bug" 04-broken-O2.txt
absent "at -O0 the same source rejects" \
  "ACCEPTED, this is the bug" 04-broken-O0.txt
present "UBSan names the undefined behaviour exactly" \
  "signed integer overflow" 04-broken-ubsan.txt
absent "-fwrapv alone makes the broken guard work again" \
  "ACCEPTED, this is the bug" 04-broken-fwrapv.txt
absent "the fixed check rejects at -O2 with no flags" \
  "ACCEPTED, this is the bug" 04-fixed-O2.txt
absent "UBSan is silent on the fixed version" \
  "signed integer overflow" 04-fixed-O2.txt

# ---------------------------------------------------------------------------
banner "05  False sharing -- adding a thread made it slower"
# No sanitizer finds this. It is not a correctness bug; the program is right and
# slow, and only measurement shows it.
compile 05-false-sharing broken c05-broken -O2
compile 05-false-sharing fixed c05-fixed -O2

capture "05-broken-timing.txt" "$BUILD/c05-broken"
capture "05-fixed-timing.txt" "$BUILD/c05-fixed"

present "the unpadded version fails to scale" "false sharing" 05-broken-timing.txt
present "the padded version scales" "^scales" 05-fixed-timing.txt

# ---------------------------------------------------------------------------
banner "06  Priority inversion -- the lowest-priority thread blocks the highest"
compile 06-priority-inversion broken c06-broken -O2
compile 06-priority-inversion fixed c06-fixed -O2

# Run to a scratch file first. This host may well be unable to grant SCHED_FIFO,
# and an inconclusive local run must not overwrite the committed evidence that
# scripts/run-case-06-container.sh produced on a host that could.
timeout 150 "$BUILD/c06-broken" >"$BUILD/06-broken.txt" 2>&1
timeout 150 "$BUILD/c06-fixed" >"$BUILD/06-fixed.txt" 2>&1

# Exit status 2 means the program declined to draw a conclusion because
# SCHED_FIFO was refused. That is not a failure of the case; it is the case
# refusing to report lock contention as priority inversion. Treating it as a
# pass, which an earlier version of this script did, is the failure.
if grep -q "INCONCLUSIVE" "$BUILD/06-broken.txt"; then
  echo "  SKIP: SCHED_FIFO refused on this host, so there is no priority to"
  echo "        invert and both variants measure the same lock contention."
  echo "        Needs CAP_SYS_NICE or a non-zero RLIMIT_RTPRIO:"
  echo "          scripts/run-case-06-container.sh"
  echo "        The committed evidence was produced that way and is left"
  echo "        untouched by this run."
elif grep -q "WAITED FAR LONGER" "$BUILD/06-broken.txt"; then
  cp "$BUILD/06-broken.txt" "$OUT/06-broken-timing.txt"
  cp "$BUILD/06-fixed.txt" "$OUT/06-fixed-timing.txt"
  record pass "the plain mutex shows priority inversion"
  if grep -q "BOUNDED by one critical section" "$BUILD/06-fixed.txt"; then
    record pass "priority inheritance bounds the wait"
  else
    record fail "priority inheritance bounds the wait"
  fi
else
  cat "$BUILD/06-broken.txt"
  record fail "the plain mutex shows priority inversion"
fi

# ---------------------------------------------------------------------------
echo
echo "==============================================================="
echo "$pass passed, $fail failed. Evidence in $OUT/"
echo "==============================================================="
[ "$fail" -eq 0 ]
