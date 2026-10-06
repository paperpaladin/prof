# prof - sampling CPU profiler on perf_event_open(2). C11, gcc, no libraries.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -g -ffile-prefix-map=$(CURDIR)=.
# The target must keep its frame pointers: the unwinder walks the RBP chain.
# -mno-omit-leaf-frame-pointer (x86-specific) is necessary but NOT sufficient:
# gcc 14 still elides the whole prologue for a leaf that needs no stack slot,
# which hides one level of the call chain. See the comment on BURN in t/workload.c.
# -fno-inline keeps hot/warm/cold/level1/level2 as distinct symbols, which is
# what makes the oracle's known ratios checkable at all.
TCFLAGS ?= -std=c11 -O1 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-inline -ffile-prefix-map=$(CURDIR)=.

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
