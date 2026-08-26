# Method

How these cases were built, and the rules they follow.

---

## The template

Every case README answers the same seven questions, in the same order:

| section | the question it answers |
|---|---|
| **Symptom** | What did someone actually report? In their words, not in diagnosis-speak. |
| **Hypothesis** | What did the symptom suggest *before* any tool was run? |
| **Evidence** | What did the tool say? Quoted, not paraphrased. |
| **Root cause** | The specific mechanism, at the level of memory or the scheduler. |
| **Fix** | The change, the alternatives rejected, and why. |
| **Regression test** | What now fails if this comes back? |
| **What made it hard** | Why the bug survived review and testing. |

The last section is the one that carries the most information and is the one
usually missing from real postmortems. A defect that was easy to find is not
worth writing up. What is worth writing up is why competent people looked
straight at it and did not see it.

---

## Rules

### The symptom comes from a person, not from a diagnosis

Every case opens with a report phrased the way a colleague would phrase it —
"it hangs about once a day", "throughput went down when we added a thread". Not
"there is a lock-order inversion".

This is not decoration. Half of what makes these bugs hard is that the symptom
points somewhere other than the cause. "Only with a lot of sensors" (case 03)
pulls toward parsing limits and configuration caps; the count is really a proxy
for how many times a vector reallocated. A case that starts from the diagnosis
has deleted the hard part.

### Evidence is quoted output, not description

Every claim is backed by a file in `evidence/`, produced by
`scripts/run-all.sh` on a real run. Where a number appears in a README it was
copied from that file.

### Say which tool was missing

Only the sanitizers are available on this machine — `perf`, `gdb`, `valgrind`,
`trace-cmd` and `bpftrace` are all absent, `perf_event_paranoid` is 2, and
installing anything needs root. Two cases would have been easier with a tool
that was not there:

- **Case 02** wants ThreadSanitizer's deadlock detector, which is a Clang
  feature that GCC's libtsan does not implement — `TSAN_OPTIONS=detect_deadlocks=1`
  is silently inert. The case builds a watchdog instead, and says plainly what
  the watchdog cannot do that the detector could: report the inverted order on
  runs where no deadlock happens.
- **Case 05** wants `perf c2c`, which attributes cache-line contention to source
  lines. The case measures throughput and reports the byte distance between
  counters instead — the same conclusion by a cruder route.

Naming the right tool and then not having it is more useful than pretending the
substitute was the plan.

### A tool that reports nothing is not a passing grade

Two cases pass their sanitizer and are still wrong:

- **Case 01** produced `expected 800000, counted 800000 -- exact` on the run
  where TSan reported the race. A test asserting on the count passes.
- **Case 04** answers *correctly* under UBSan, because the instrumentation
  defeats the optimisation that causes the misbehaviour. The diagnostic line is
  the finding; the restored behaviour is an artefact.

So the regression tests assert on **what the tool said**, not on program output,
wherever program output can be right for the wrong reason.

### Refuse to conclude when the conditions do not support it

Case 06 needs `SCHED_FIFO`. If it is refused, both variants measure ordinary
lock contention and would appear to disagree about nothing — so the programs
read the policy back with `pthread_getschedparam`, print `INCONCLUSIVE`, and
exit 2 rather than report a number.

An earlier version did not, and on WSL2 confidently reported "priority
inversion" at 6398 µs with a "fixed" version at 6224 µs. Both were meaningless.
That failure mode — a demonstration that measures nothing while appearing to
succeed — is the one worth engineering against, because nothing about the output
looks wrong.

The same discipline appears in the calibration: it measures its own result and
aborts if the critical section is off by more than 2× from target. A calibration
is a prediction, and predictions get checked.

### Both halves of a comparison must be the same experiment

In case 06, `broken.cpp` and `fixed.cpp` differ only in the mutex type. The
workload, the duty cycles, the calibration and the thread priorities are
identical. If the workload differed, the comparison would not be a comparison.

### Report ratios, not absolute numbers, where the machine decides

Case 05 asserts that four threads scale, not that they reach 972 M
increments/second. The absolute figure depends on core count, cache topology,
and whether the host is virtualised. The ratio survives; the megahertz do not.

---

## Reproducing a bug is usually harder than fixing it

Case 06's underlying defect is four lines. Getting an experiment that
demonstrates it honestly took three attempts, and **each failure produced
plausible output**:

1. Priorities were refused → both variants reported the same number.
2. The critical section was defined in wall-clock time, so preemption could not
   lengthen it → "no significant inversion", correctly, forever.
3. The hog's idle window was longer than the critical section, so the holder
   always finished before interference arrived → "no significant inversion"
   again.

None of these looked like errors. Two of them looked like the case simply not
reproducing on this host, which is exactly the conclusion someone in a hurry
would have accepted and moved on from.

The general shape: **when a demonstration reports nothing, the first hypothesis
is that the demonstration is broken, not that the phenomenon is absent.**

---

## What is not here

- **No RPN scoring, no severity matrix.** These are six teaching cases, not a
  hazard analysis. (The FMEA in the `safeedge` repository deliberately omits RPN
  too, for reasons documented there.)
- **No claim that these are the six most important bugs.** They are six with
  distinct mechanisms — memory ordering, lock ordering, object lifetime,
  optimiser assumptions, cache coherence, scheduling — chosen so that no two are
  found by the same tool.
- **No timing numbers presented as characterising a machine.** The host is
  virtualised. Case 05's ratios are meaningful; its absolute throughput is not a
  benchmark of anything, and case 06's microsecond figures characterise a
  container on a Windows host, not real-time hardware.
