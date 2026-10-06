/* workload - the external oracle.
 *
 * The point of this file is that its CPU profile is known BEFORE the profiler
 * runs, by construction rather than by measurement:
 *
 *   hot()  burns 100 units      ~90.1% of user CPU
 *   warm() burns  10 units      ~ 9.0%
 *   cold() burns   1 unit       ~ 0.9%
 *
 * and hot() is only ever reached through main -> level1 -> level2 -> hot, so a
 * correct unwinder must produce exactly that chain and nothing else.
 *
 * Each of the three has its own copy of the loop (via the BURN macro) instead
 * of calling a shared burn() helper. If they shared one, every sample would
 * land in the helper and the leaf symbol would be the same for all three --
 * which would make the oracle untestable rather than making the profiler wrong.
 *
 * The arithmetic is a serial dependency chain (x feeds back into itself) so the
 * compiler cannot vectorise it into a different cost ratio, and the result is
 * written to a volatile sink so it cannot be dead-coded away.
 */

#include <stdio.h>
#include <stdlib.h>

static volatile double sink;

#define UNIT 3000000L

#define BURN(units)                                    \
	do {                                           \
		double x = 1.0;                        \
		long n = (units) * UNIT;               \
		for (long i = 0; i < n; i++)           \
			x = x * 0.9999 + 0.5;          \
		sink = x;                              \
	} while (0)

__attribute__((noinline)) static void cold(void) { BURN(1); }
__attribute__((noinline)) static void warm(void) { BURN(10); }
__attribute__((noinline)) static void hot(void) { BURN(100); }

/* The call chain the profiler has to reproduce. */
__attribute__((noinline)) static void level2(void) { hot(); }
__attribute__((noinline)) static void level1(void) { level2(); }

int main(int argc, char **argv) {
	long reps = argc > 1 ? atol(argv[1]) : 1;

	/* Transparency check material: exact stdout, and a distinctive exit code
	 * that the profiler must pass through untouched. */
	printf("workload: ok reps=%ld\n", reps);
	fflush(stdout);

	for (long r = 0; r < reps; r++) {
		level1(); /* -> level2 -> hot */
		warm();
		cold();
	}
	return 7;
}
