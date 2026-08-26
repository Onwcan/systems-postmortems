#!/usr/bin/env bash
#
# Runs case 06 with real-time scheduling actually available.
#
# WSL2 and most CI runners refuse SCHED_FIFO, and on WSL2 the *hard*
# RLIMIT_RTPRIO is 0, so it cannot be raised from inside the guest at all. A
# container can be granted what the host will not give a process.
#
# What actually grants it is worth being precise about, because the obvious
# guess is wrong. This script measures it rather than asserting it -- see the
# matrix at the end and evidence/06-scheduling-matrix.txt.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/evidence}"
mkdir -p "$OUT"

RUN_TIMEOUT="${RUN_TIMEOUT:-130}"

docker build -t postmortems-c06 "$ROOT"

echo
echo "=== plain std::mutex (no priority inheritance) ==="
docker run --rm --cap-add=SYS_NICE --ulimit rtprio=99 --ulimit memlock=-1 \
  postmortems-c06 timeout -s KILL "$RUN_TIMEOUT" c06-broken 2>&1 |
  tee "$OUT/06-broken-timing.txt" || true

echo
echo "=== PTHREAD_PRIO_INHERIT ==="
docker run --rm --cap-add=SYS_NICE --ulimit rtprio=99 --ulimit memlock=-1 \
  postmortems-c06 timeout -s KILL "$RUN_TIMEOUT" c06-fixed 2>&1 |
  tee "$OUT/06-fixed-timing.txt" || true

# Which container settings actually permit SCHED_FIFO?
#
# This started as an assertion in a comment -- "a container hardened with
# --cap-drop=ALL cannot run a real-time control loop" -- and the first run of
# this script disproved it: --cap-drop=ALL with --ulimit rtprio=99 got all three
# threads scheduled. The table below is what the machine says.
#
# CAP_SYS_NICE is needed to exceed RLIMIT_RTPRIO. Raising the rlimit directly
# removes the need for it. Either one alone is sufficient; Docker's default
# capability set includes neither, which is why the plain run is refused.
{
  echo "Does SCHED_FIFO work? One row per container configuration."
  echo
  printf '%-34s %-16s %s\n' "docker flags" "RLIMIT_RTPRIO" "SCHED_FIFO"
  printf '%-34s %-16s %s\n' "----------------------------------" "----------------" "-------------"

  probe() {
    local label="$1"
    shift
    local output
    output="$(docker run --rm "$@" postmortems-c06 \
      timeout -s KILL "$RUN_TIMEOUT" c06-broken 2>&1 || true)"
    local rtprio granted
    rtprio="$(echo "$output" | sed -n 's/^RLIMIT_RTPRIO: *//p' | head -1)"
    granted="$(echo "$output" | sed -n 's/^SCHED_FIFO granted: *//p' | head -1)"
    printf '%-34s %-16s %s\n' "$label" "${rtprio:-?}" "${granted:-?}"
  }

  probe "--cap-add=SYS_NICE --ulimit rtprio" --cap-add=SYS_NICE --ulimit rtprio=99
  probe "--cap-drop=ALL     --ulimit rtprio" --cap-drop=ALL --ulimit rtprio=99
  probe "--cap-add=SYS_NICE" --cap-add=SYS_NICE
  probe "--cap-drop=ALL" --cap-drop=ALL
  probe "(docker defaults)"

  echo
  echo "Either CAP_SYS_NICE or a non-zero RLIMIT_RTPRIO is sufficient."
  echo "Docker's default capability set provides neither."
} | tee "$OUT/06-scheduling-matrix.txt"

echo
echo "Evidence written to $OUT/"
