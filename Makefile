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
# No frame pointers at all: what every -O2 build and every distro binary looks
# like. The saved-RBP walk yields depth-1 stacks here and only .eh_frame CFI
# can recover the call chain, which is what gdb's backtrace is used to check.
# -fno-optimize-sibling-calls because a tail call physically removes the frame
# from the stack: gdb reconstructs those from .debug_info call-site data, prof
# cannot, and a test comparing the two has to compare real frames. See README.
NFPFLAGS ?= -std=c11 -O2 -g -fomit-frame-pointer -fno-inline -fno-optimize-sibling-calls -ffile-prefix-map=$(CURDIR)=.
# The other direction: frame pointers but NO unwind tables, so the per-mapping
# unwinder choice has to fall back to the RBP walk for this object while still
# using CFI for libc.
NOCFIFLAGS ?= -std=c11 -O1 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-inline -fno-asynchronous-unwind-tables -ffile-prefix-map=$(CURDIR)=.
# ASan builds of prof itself. The two planted over-reads are deliberate: they
# are what proves the .eh_frame copy and the sample record buffer are sized to
# their data, so that a real overrun there is visible rather than landing in
# slack. t/run.sh requires ASan to catch both.
ASANFLAGS ?= -std=c11 -O1 -g -fsanitize=address -ffile-prefix-map=$(CURDIR)=.

all: prof t/workload t/workload_nofp t/workload_nocfi t/cfi_probe

prof: prof.c
	$(CC) $(CFLAGS) -o $@ $<

t/workload: t/workload.c
	$(CC) $(TCFLAGS) -o $@ $<

t/workload_nofp: t/workload.c
	$(CC) $(NFPFLAGS) -o $@ $<

t/workload_nocfi: t/workload.c
	$(CC) $(NOCFIFLAGS) -o $@ $<

# Hand-written CFI for the opcodes no installed binary emits. gas encodes the
# directives, readelf says what they mean, prof has to agree: see the file.
t/cfi_probe: t/cfi_probe.s
	$(CC) -ffile-prefix-map=$(CURDIR)=. -o $@ $<

prof-asan: prof.c
	$(CC) $(ASANFLAGS) -o $@ $<

prof-asan-plant1: prof.c
	$(CC) $(ASANFLAGS) -DPROF_PLANT_OVERREAD=1 -o $@ $<

prof-asan-plant2: prof.c
	$(CC) $(ASANFLAGS) -DPROF_PLANT_OVERREAD=2 -o $@ $<

test: all prof-asan prof-asan-plant1 prof-asan-plant2
	@sh t/run.sh

clean:
	rm -f prof prof-asan prof-asan-plant1 prof-asan-plant2 \
	      t/workload t/workload_nofp t/workload_nocfi t/cfi_probe \
	      t/*.out t/*.folded

.PHONY: all test clean
