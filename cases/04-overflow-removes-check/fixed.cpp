// SPDX-License-Identifier: Apache-2.0
//
// Fix: do not let the check overflow. Subtract instead of adding.
//
// `length > kBufferSize - offset` decides the same question with operands that
// cannot overflow, because `offset` has already been shown to lie within the
// buffer. No undefined behaviour, so no licence to discard anything, so debug
// and release agree -- and so does UBSan, which is silent here.
//
// The general rule: **a bounds check must not itself be capable of
// overflowing.** A guard written as `end < offset` is asking wraparound to
// catch undefined behaviour, and wraparound is not something the language
// promises. Compilers delete such guards routinely; nothing warns.
//
// Two weaker alternatives, worth knowing but not what this uses:
//
//   -fwrapv           defines signed overflow as wrapping, so `end < offset`
//                     starts working. Verified in evidence/04-broken-fwrapv.txt.
//                     It fixes one file's symptom by changing the language for
//                     the whole translation unit, and it silently stops
//                     applying the moment someone drops the flag.
//
//   widen to int64_t  makes the addition unable to overflow at these
//                     magnitudes. Correct, but it moves the boundary rather
//                     than removing it, and the reader still has to prove no
//                     input reaches the new one.
//
// Subtracting needs neither a flag nor a proof about magnitudes.

#include <cstdio>

namespace {

constexpr int kBufferSize = 1024;

__attribute__((noinline)) bool fits(int offset, int length) {
  if (offset < 0 || length < 0) {
    return false;
  }
  if (offset > kBufferSize) {
    return false;
  }
  // Cannot overflow: offset is now known to be in [0, kBufferSize], so the
  // right-hand side is in [0, kBufferSize] and both sides are non-negative.
  return length <= kBufferSize - offset;
}

}  // namespace

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

  // The boundary itself, which is where off-by-one lives. A subtracting check
  // is only correct if it is also correct at exactly the buffer size.
  std::printf("fits(1024, 0)                 -> %s\n", fits(1024, 0) ? "yes" : "no");
  std::printf("fits(1023, 1)                 -> %s\n", fits(1023, 1) ? "yes" : "no");
  std::printf("fits(1023, 2)                 -> %s\n", fits(1023, 2) ? "yes" : "no");
  std::printf("fits(1024, 1)                 -> %s\n", fits(1024, 1) ? "yes" : "no");

  return huge ? 1 : 0;
}
