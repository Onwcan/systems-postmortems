# systems-postmortems

Six classic C++ systems defects, each reproduced deliberately, diagnosed with
real tooling, fixed, and written up.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License](https://img.shields.io/badge/license-Apache--2.0-green)

Every case is two files — `broken.cpp` and `fixed.cpp` — plus a postmortem and
the captured tool output that supports it. Nothing is asserted that
`evidence/` does not contain.

```bash
scripts/run-all.sh
```

Builds every case with the flags its demonstration requires, runs it, captures
the output, and checks each expected finding. **14 checks, all passing**. Case
05 compares padded and unpadded throughput at the same thread count instead of
assuming the host can supply four cores; case 06 reports SKIP on hosts that
cannot grant real-time scheduling and says how to run it where they can.

---

## The cases

| # | Defect | Found by | The number |
|---|---|---|---|
| [01](cases/01-dropped-counters/) | Data race on a plain counter | ThreadSanitizer | Counted **exactly right** on the run that reported the race |
| [02](cases/02-lock-order-inversion/) | Deadlock from swapped lock order | A hand-built watchdog | `cycle: alice -> bob -> alice` |
| [03](cases/03-dangling-reference/) | Reference held across `push_back` | AddressSanitizer | `heap-use-after-free`, freed inside `new_allocator.h` |
| [04](cases/04-overflow-removes-check/) | Optimiser deletes an overflow guard | UBSan + disassembly | Two comparisons in the source, **one** in the object code |
| [05](cases/05-false-sharing/) | Counters sharing a cache line | Measurement | **0.46×** with four threads → **3.92×** padded |
| [06](cases/06-priority-inversion/) | Low-priority thread blocks a control loop | Measurement, in a container | **15246 µs** → **5216 µs** |

No two are found by the same tool. That is the selection criterion — the set is
chosen to cover distinct mechanisms (memory ordering, lock ordering, object
lifetime, optimiser assumptions, cache coherence, scheduling), not to be the six
most common bugs.

---

## Three findings worth the click

**A wrong program can print the right answer.** Case 01's racy counter reported
`expected 800000, counted 800000 -- exact` on the very run where ThreadSanitizer
reported the data race. A unit test asserting on the count passes. The race is
still there, and only a tool that reports *the possibility* rather than *the
occurrence* catches it.

**A sanitizer can hide the bug it diagnoses.** Case 04's bounds check is deleted
by the optimiser at `-O2` and accepts a write 2 GB past a 1 KB buffer. Under
`-fsanitize=undefined` the same binary answers *correctly* — the instrumentation
defeats the optimisation. "It passes under the sanitizer" is not "it works". The
disassembly settles it: the guard is not bypassed, it is absent.

**A demonstration can measure nothing while appearing to succeed.** Case 06's
first version ignored whether `SCHED_FIFO` was actually granted. On WSL2, where
the hard `RLIMIT_RTPRIO` is 0, it reported "priority inversion" at 6398 µs
against a "fixed" 6224 µs. Both were ordinary lock contention; there were no
priorities, so nothing was inverted. Nothing in the output looked wrong. The
programs now read the policy back and print `INCONCLUSIVE` rather than a number.

---

## Case 06 needs real-time scheduling

```bash
scripts/run-case-06-container.sh
```

WSL2 and most CI runners refuse `SCHED_FIFO`, and on WSL2 the *hard*
`RLIMIT_RTPRIO` is 0 and cannot be raised from inside. A container can be
granted what the host will not give a process.

What grants it is not the obvious answer. The harness measures it rather than
asserting it:

```
docker flags                       RLIMIT_RTPRIO    SCHED_FIFO
---------------------------------- ---------------- -------------
--cap-add=SYS_NICE --ulimit rtprio soft 99, hard 99 3 of 3 threads
--cap-drop=ALL     --ulimit rtprio soft 99, hard 99 3 of 3 threads
--cap-add=SYS_NICE                 soft 0, hard 0   3 of 3 threads
--cap-drop=ALL                     soft 0, hard 0   0 of 3 threads
(docker defaults)                  soft 0, hard 0   0 of 3 threads
```

**Either `CAP_SYS_NICE` or a non-zero `RLIMIT_RTPRIO` is sufficient**; Docker's
defaults provide neither. This table exists because the first version of the
harness claimed in a comment that `--cap-drop=ALL` made real-time scheduling
impossible, and the first run disproved it.

---

## Working without the right tool

Only the sanitizers are installed here. `perf`, `gdb`, `valgrind`, `trace-cmd`
and `bpftrace` are absent, `perf_event_paranoid` is 2, and installing anything
needs root.

Two cases name the tool they wanted and explain what the substitute cannot do:

- **Case 02** wants ThreadSanitizer's deadlock detector — a Clang feature GCC's
  libtsan does not implement, where `TSAN_OPTIONS=detect_deadlocks=1` is
  silently inert. The case builds a `TracedLock` wrapper and a watchdog that
  prints the wait-for cycle. What it loses: TSan reports the inverted order on
  runs where *no deadlock occurs*, and against a once-a-day bug that difference
  is the entire problem.
- **Case 05** wants `perf c2c`. It measures throughput and reports the byte
  distance between counters instead.

---

## Further reading

[`docs/method.md`](docs/method.md) — the template every case follows, the rules
about evidence, and why reproducing a bug is usually harder than fixing it
(case 06 took three attempts, and **each failure produced plausible output**).

Related: [`safeedge`](https://github.com/Onwcan/safeedge) — a failsafe industrial
edge runtime, where several of these defect classes are what the design is
guarding against. [`rt-latency-lab`](https://github.com/Onwcan/rt-latency-lab) —
real-time latency measurement, and knowing when a measurement is worthless.

---

## Licence

Apache-2.0.
