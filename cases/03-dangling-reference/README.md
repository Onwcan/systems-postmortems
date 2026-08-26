# 03 — Dangling reference

A reference into a `std::vector`, held across a `push_back`. It works until the
vector outgrows its capacity.

## Symptom

> "The last sensor in the list occasionally reads as garbage — huge values,
> sometimes zero. Only when we have a lot of sensors configured. With four or
> five it never happens."

## Hypothesis

"Only with a lot of them" is a statement about *growth*, not about the data. The
values themselves are unremarkable; what changes with count is how many times
the container has had to reallocate. That points at a reference or pointer into
storage that has since moved.

## Evidence

`evidence/03-broken-asan.txt` — AddressSanitizer:

```
==521==ERROR: AddressSanitizer: heap-use-after-free on address 0x6fa0de1e0030
READ of size 8 at 0x6fa0de1e0030 thread T0
    #0 in main cases/03-dangling-reference/broken.cpp:39

0x6fa0de1e0030 is located 32 bytes inside of 40-byte region [...]
freed by thread T0 here:
    #0 in operator delete(void*, unsigned long)
    #1 in deallocate /usr/include/c++/15/bits/new_allocator.h:172
```

ASan gives all three facts at once: the read at line 39, that the memory was
freed, and — through the `new_allocator.h` frame — that the deallocation came
from the vector's own reallocation rather than from anything in application
code. That last frame is what turns "use after free" into "the container moved
under you".

## Root cause

```cpp
Sensor& first = sensors.front();   // valid here
for (int i = 0; i < 64; ++i) {
  sensors.push_back(...);          // may reallocate; `first` now dangles
}
std::printf("%s", first.name.c_str());
```

`push_back` invalidates all references, pointers, and iterators into the vector
whenever it reallocates. It reallocates only when size reaches capacity — which
is why the bug is invisible at small scale. With four sensors the initial
allocation has room and the reference stays valid, so the code appears correct
and stays in the codebase.

The reference gives no indication of any of this. It looks like a live object
right up to the moment it is read.

## Fix

Hold an **index** rather than a reference, and re-fetch after mutating.

The tempting alternative is `sensors.reserve(65)` so no reallocation occurs.
That works, and it is the wrong fix: it makes the correctness of this reference
depend on a capacity computation that lives somewhere else, is not checked by
anything, and stops being true the moment someone configures a sixty-sixth
sensor. `reserve` is an optimisation. Using it as a lifetime guarantee is how
the bug comes back a year later in a form nobody connects to this one.

An index cannot dangle.

## Regression test

`scripts/run-all.sh` builds both variants under `-fsanitize=address` and
requires ASan to report `heap-use-after-free` in the broken one and to be silent
on the fixed one.

Note that the broken program does **not** crash without ASan — it prints
plausible-looking garbage and exits 0. Asserting on its output would be
asserting on undefined behaviour.

## What made it hard

Scale-dependence that inverts the usual debugging instinct. The natural response
to "it breaks with many sensors" is to look at what is different about the many
— a parsing limit, an overflow, a configuration cap. The count is not the cause;
it is a proxy for how many times the vector has reallocated, and nothing in the
symptom points at that.

It also survives review. `Sensor& first = sensors.front();` is idiomatic, and the
`push_back` loop is idiomatic, and the two are separated by enough lines that
their interaction is not on screen at the same time.
