# 01 — Dropped counters

A metrics counter incremented from four threads. Sometimes the total is short.
Sometimes it is exactly right, which is worse.

## Symptom

> "Our request counter reads low. Not by much — a few hundred out of a few
> hundred thousand — and not every time. Some days it matches exactly. We've
> checked the arithmetic twice."

## Hypothesis

Arithmetic that is right at the source level but wrong at run time under
concurrency means the operation is not atomic. `counter++` on a plain
`uint64_t` is a load, an add, and a store; two threads can interleave those and
one increment is lost.

The intermittency fits: losing an increment requires two threads to be inside
that three-step sequence at the same time, which is rare per attempt and certain
over millions of attempts.

## Evidence

`evidence/01-broken-tsan.txt` — ThreadSanitizer:

```
WARNING: ThreadSanitizer: data race (pid=465)
  Read of size 8 at 0x555555558020 by thread T2:
    #0 worker cases/01-dropped-counters/broken.cpp:29
  Previous write of size 8 at 0x555555558020 by thread T1:
    #0 worker cases/01-dropped-counters/broken.cpp:29
SUMMARY: ThreadSanitizer: data race cases/01-dropped-counters/broken.cpp:29 in worker
```

Same address, same line, one thread reading while another writes.

**The run that produced this report counted correctly:**

```
expected 800000, counted 800000  -- exact this run (the race is still there)
ThreadSanitizer: reported 2 warnings
```

That line is the point of the case. A test asserting `counted == expected`
passes here. The program is still wrong, and the only thing that says so is the
sanitizer, which reports the *race* rather than the *symptom* — it does not
require the bad interleaving to actually occur, only for the unsynchronised
access to be possible.

## Root cause

`counter += 1` on a non-atomic object, executed concurrently, is a data race,
and a data race is undefined behaviour rather than merely a lost update. The
compiler is entitled to assume it does not happen — it may keep the value in a
register across the loop, so the losses can be far larger than "one increment
per unlucky interleaving" suggests.

## Fix

`std::atomic<std::uint64_t>` with `fetch_add(1, std::memory_order_relaxed)`.

`relaxed` is deliberate and is the interesting half. A counter needs
indivisibility, not ordering: nothing else is published through this variable,
and no reader infers anything about other memory from its value. `relaxed`
provides the indivisibility and permits the compiler and hardware to reorder
around it, which on x86-64 compiles to the same `lock xadd` as `seq_cst` but on
weakly-ordered targets (aarch64, RISC-V) omits barriers that would otherwise be
emitted for nothing.

Reaching for `seq_cst` because it is the default is how atomics get a reputation
for being slow.

## Regression test

`scripts/run-all.sh` builds both variants under `-fsanitize=thread` and requires
TSan to report a race in the broken one and to be **silent** on the fixed one.

Asserting on the count would not be a regression test: this case demonstrates
that the count can be right while the program is wrong.

## What made it hard

The correct answer appearing intermittently. A bug that produces wrong output
every time gets found in an afternoon. A bug that produces wrong output on 1 run
in 20 gets attributed to the metrics pipeline, to a dropped UDP packet, to
"eventual consistency" — to anything except the counter, because the counter was
checked and it was fine.
