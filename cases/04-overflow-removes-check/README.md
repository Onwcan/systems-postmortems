# 04 — The overflow check the compiler deleted

A bounds check that works in the debug build and does not work in release. Same
source. Same input. Opposite answers.

## Symptom

> "The bounds check works in our debug build and does not work in release. Same
> source, same test input. We assumed a compiler bug and opened a ticket
> against GCC."

## Hypothesis

Debug and release are permitted to disagree in exactly one circumstance:
undefined behaviour. Everywhere else the compiler must preserve observable
behaviour, so a genuine optimiser bug is far less likely than UB in the code
being optimised.

The check adds two `int`s. If that addition can overflow, the whole thing is UB
and the disagreement is explained.

## Evidence

Four builds of one file (`evidence/04-broken-*.txt`).

| build | `fits(2000000000, 2000000000)` |
|---|---|
| `-O0` | `no` |
| `-O2` | **`yes` ← the bug** |
| `-O2 -fsanitize=undefined` | `no` (plus a diagnostic) |
| `-O2 -fwrapv` | `no` |

UBSan names it exactly:

```
broken.cpp:38:13: runtime error: signed integer overflow:
2000000000 + 2000000000 cannot be represented in type 'int'
```

The strongest evidence is the object code
(`evidence/04-broken-disassembly.txt`). The guard is not bypassed — it is
**absent**:

```
=== -O0: the comparison is present ===
    1190:  add    %edx,%eax
    1192:  mov    %eax,-0x4(%rbp)
    1198:  cmp    -0x14(%rbp),%eax      <-- end < offset
    119b:  jge    11a4
    119d:  mov    $0x0,%eax             <-- return false
    11a4:  cmpl   $0x400,-0x4(%rbp)

=== -O2: the comparison is gone ===
    1220:  mov    %edi,%edx
    1222:  xor    %eax,%eax
    1224:  or     %esi,%edx
    1226:  js     1233
    1228:  add    %esi,%edi
    122a:  cmp    $0x400,%edi           <-- only the buffer-size test survives
    1230:  setle  %al
    1233:  ret
```

Two comparisons in the source; one in the object code.

### The sanitizer also hides the bug

Note row three: under UBSan the program answers **`no`** — correctly. The
instrumentation defeats the optimisation that causes the misbehaviour, so the
program *works* under the tool that diagnoses it.

"It passes under the sanitizer" therefore does not mean "it works". The
diagnostic line is the finding; the restored behaviour is an artefact.

## Root cause

```cpp
const int end = offset + length;   // overflows
if (end < offset) return false;    // deleted at -O2
return end <= kBufferSize;
```

Signed overflow is undefined behaviour, so the compiler may assume it never
occurs. Given that assumption, `offset + length` is always `>= offset` for
non-negative `length`, so `end < offset` is provably false, so the branch is
dead code and is removed.

The guard was written specifically to catch overflow, and it is removed
*because* overflow is what it catches. Nothing warns: `-Wall -Wextra` say
nothing, and `-Wstrict-overflow=3` and `=5` were both silent here too.

## Fix

Subtract instead of adding:

```cpp
if (offset < 0 || length < 0) return false;
if (offset > kBufferSize) return false;
return length <= kBufferSize - offset;   // cannot overflow
```

After the second check, `offset` is known to be in `[0, kBufferSize]`, so
`kBufferSize - offset` is in `[0, kBufferSize]` and both sides are non-negative.
No undefined behaviour, so nothing to exploit, so debug and release agree.

**The general rule: a bounds check must not itself be capable of overflowing.**
A guard that relies on wraparound is asking undefined behaviour to catch
undefined behaviour.

Two alternatives, both weaker:

- **`-fwrapv`** defines signed overflow as wrapping and makes the original guard
  work (row four above). It fixes one file's symptom by changing the language
  for an entire translation unit, and it stops applying silently the moment
  someone drops the flag from a build file.
- **Widening to `int64_t`** makes the addition unable to overflow at these
  magnitudes. Correct, but it moves the boundary rather than removing it, and
  the reader still has to prove no input reaches the new one.

Subtracting needs neither a flag nor a proof about magnitudes.

## Regression test

`scripts/run-all.sh` builds the broken file four ways and requires all four
answers above, and builds the fixed file at `-O2 -fsanitize=undefined`
requiring both a rejection and sanitizer silence. The fixed variant also checks
the boundary itself — `fits(1024, 0)`, `fits(1023, 1)`, `fits(1023, 2)`,
`fits(1024, 1)` — because a subtracting check is only correct if it is also
correct at exactly the buffer size.

## What made it hard

It presents as a compiler bug, and that framing sends the investigation
somewhere with no bug in it. The team's evidence for "compiler bug" was
genuinely good — identical source, identical input, different behaviour — and
the inference is only wrong because UB is the one case where that reasoning
fails.

Then the debugger lies. Attaching one, or adding a print, or building with `-g`
all push toward `-O0`, where the check *works*. The bug disappears exactly when
you look at it.

And the guard reads as careful. Code with an explicit overflow check looks more
rigorous than code without one, so it draws less scrutiny, not more.
