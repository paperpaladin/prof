/* deep - a target whose stack depth is known by construction.
 *
 * t/workload.c has a four-frame chain, which proves an unwinder can walk a
 * stack but says nothing about what happens to a deep one. Three limits only
 * show up here:
 *
 *   MAX_DEPTH   128 frames per sample, past which prof stops
 *   STACK_BYTES 8192 bytes of stack copied per sample, past which the frames
 *               we would need were never captured and the sample counts as a
 *               truncated stack
 *   the drain loop, which has to keep up with samples that are 8 KB each
 *               rather than with the small ones a shallow stack produces
 *
 * rec() recurses exactly as far as it is told and burns CPU only at the
 * bottom, so every sample lands in the deepest frame and the correct stack is
 * "main, rec x depth, burn". The volatile local keeps the frame from being
 * elided; -fno-optimize-sibling-calls keeps the recursion from becoming a
 * loop. gdb's backtrace at the bottom is the oracle for how many frames there
 * really are.
 */

#include <stdio.h>
#include <stdlib.h>

static volatile double sink;

__attribute__((noinline)) static void burn(long units) {
	volatile double keep = 1.0;
	double x = keep;
	long n = units * 3000000L;
	for (long i = 0; i < n; i++) x = x * 0.9999 + 0.5;
	keep = x;
	sink = keep;
}

__attribute__((noinline)) static void rec(int n, long units) {
	volatile int keep = n;
	if (keep > 0) rec(keep - 1, units);
	else burn(units);
}

int main(int argc, char **argv) {
	int depth = argc > 1 ? atoi(argv[1]) : 50;
	long units = argc > 2 ? atol(argv[2]) : 20;

	printf("deep: ok depth=%d\n", depth);
	fflush(stdout);
	rec(depth, units);
	return 0;
}
