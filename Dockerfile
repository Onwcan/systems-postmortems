# Case 06 needs real-time scheduling. WSL2 and most CI runners refuse it:
# RLIMIT_RTPRIO is 0 and on WSL2 the *hard* limit is 0, so it cannot be raised
# from inside. A container can be granted what the host will not give a process.
#
# This image exists for that one purpose. The other five cases need nothing
# special and run fine with scripts/run-all.sh.

FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ binutils && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /opt/postmortems
COPY cases/ cases/
COPY scripts/ scripts/

RUN g++ -std=c++20 -O2 cases/06-priority-inversion/broken.cpp -o /usr/local/bin/c06-broken -lpthread && \
    g++ -std=c++20 -O2 cases/06-priority-inversion/fixed.cpp  -o /usr/local/bin/c06-fixed  -lpthread

# Prints both variants back to back, which is the comparison the case is about.
CMD ["/bin/sh", "-c", "echo '=== plain std::mutex ==='; c06-broken; echo; echo '=== PTHREAD_PRIO_INHERIT ==='; c06-fixed"]
