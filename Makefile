# prof - sampling CPU profiler on perf_event_open(2). C11, gcc, no libraries.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -g
# The target must keep its frame pointers: the unwinder walks the RBP chain.
# -fno-inline keeps hot/warm/cold/level1/level2 as distinct symbols, which is
# what makes the oracle's known ratios checkable at all.
TCFLAGS ?= -std=c11 -O1 -g -fno-omit-frame-pointer -fno-inline

all: prof t/workload

prof: prof.c
	$(CC) $(CFLAGS) -o $@ $<

t/workload: t/workload.c
	$(CC) $(TCFLAGS) -o $@ $<

test: all
	@sh t/run.sh

clean:
	rm -f prof t/workload t/*.out t/*.folded

.PHONY: all test clean
