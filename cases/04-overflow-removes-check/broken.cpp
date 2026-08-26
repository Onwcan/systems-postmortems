// SPDX-License-Identifier: Apache-2.0
//
// Symptom as reported: "the bounds check works in our debug build and does not
// work in release. Same source, same input, opposite answer. We assumed a
// compiler bug and opened a ticket against GCC."
//
// It is not a compiler bug. `offset + length` overflows, signed overflow is
// undefined behaviour, and undefined behaviour is precisely where debug and
// release are permitted to disagree. The compiler is allowed to assume overflow
// never happens; from that assumption `offset + length < offset` is always
// false for non-negative length, so the guard is dead code and gets deleted.
//
// The guard is not merely ineffective. It is *gone* -- see the disassembly in
// evidence/04-broken-disassembly.txt, where the whole function compiles to a
// single comparison against the buffer size.
//
// This program does not perform the out-of-range write. It only reports the
// decision the bounds check reached, so the failure is legible instead of being
// a segfault with the output buffer lost.

#include <cstdio>

namespace {

constexpr int kBufferSize = 1024;

/// Does a write of `length` bytes at `offset` fit inside the buffer?
///
/// Reads as though it is careful. It has an explicit overflow guard, which is
/// more than most such checks have. That guard is the part that disappears.
///
/// noinline so the generated code can be read on its own in the disassembly;
/// the bug does not depend on it.
__attribute__((noinline)) bool fits(int offset, int length) {
  if (offset < 0 || length < 0) {
    return false;
  }
  const int end = offset + length;  // <-- overflows
  if (end < offset) {
    return false;  // <-- deleted at -O2: "overflow cannot happen, so end >= offset"
  }
  return end <= kBufferSize;
}

}  // namespace

// volatile so the values reach `fits` at run time. With literals the whole call
// folds away at -O2 and the disagreement is hidden behind constant propagation
// -- which is itself worth knowing, because it is why a unit test with hardcoded
// arguments can pass while the same code fails on real input.
volatile int v_offset = 2'000'000'000;
volatile int v_length = 2'000'000'000;

int main() {
  std::printf("buffer size: %d bytes\n", kBufferSize);

  const bool small = fits(0, 16);
  std::printf("fits(0, 16)                   -> %s\n", small ? "yes" : "no");

  const int offset = v_offset;
  const int length = v_length;
  const bool huge = fits(offset, length);
  std::printf("fits(%d, %d) -> %s\n", offset, length,
              huge ? "yes  <-- ACCEPTED, this is the bug" : "no");

  if (huge) {
    std::printf(
        "\nthe check approved a write running ~2 GB past a 1 KB buffer.\n"
        "compile this same file at -O0 and it answers no.\n");
  }
  return huge ? 1 : 0;
}
