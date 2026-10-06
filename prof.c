/* prof - a sampling CPU profiler built directly on perf_event_open(2).
 *
 * Usage: prof [-F HZ] [-o FILE] -- ./prog args...
 *
 * What it does so far:
 *   fork/exec the target with a perf event already attached (enable_on_exec),
 *   choose a sampling event this kernel will actually give an unprivileged
 *   user, and report which one that was.
 *
 * No libelf, no libunwind, no libdw. C11 + Linux headers only.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static const char *prog = "prof";

static void die(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s: ", prog);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

/* ------------------------------------------------------------------ *
 * perf_event_open plumbing.
 * ------------------------------------------------------------------ */

static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu) {
	return syscall(SYS_perf_event_open, a, pid, cpu, -1, 0UL);
}

/* ------------------------------------------------------------------ */

/* So a failure message can quote the actual sysctl values instead of making the
 * user go and look them up. Two static buffers because the diagnostic prints
 * two of these in one fprintf. */
static const char *sysctl_str(const char *path, int slot) {
	static char v[2][24];
	snprintf(v[slot], sizeof v[slot], "?");
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(v[slot], sizeof v[slot], f)) {
			char *nl = strchr(v[slot], '\n');
			if (nl) *nl = 0;
		}
		fclose(f);
	}
	return v[slot];
}

static void usage(void) {
	fprintf(stderr,
	        "usage: %s [-F HZ] -- ./prog [args...]\n"
	        "  -F HZ      sample frequency, default 997\n",
	        prog);
	exit(2);
}

int main(int argc, char **argv) {
	int hz = 997;
	int i = 1;

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		else if (!strcmp(argv[i], "-F") && i + 1 < argc) hz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) usage();
		else if (argv[i][0] == '-') die("unknown option %s (try -h)", argv[i]);
		else break; /* bare program name, no -- */
	}
	if (i >= argc) usage();
	if (hz < 1) die("-F must be >= 1");
	char **target = argv + i;

	/* Hand the child a pipe to block on, so the perf events are attached and
	 * armed before it execs. enable_on_exec then starts counting at the exact
	 * instant the new image takes over, so we never sample our own fork. */
	int pfd[2];
	if (pipe(pfd) < 0) die("pipe: %s", strerror(errno));

	pid_t pid = fork();
	if (pid < 0) die("fork: %s", strerror(errno));
	if (pid == 0) {
		char c;
		close(pfd[1]);
		if (read(pfd[0], &c, 1) != 1) _exit(126);
		close(pfd[0]);
		execvp(target[0], target);
		fprintf(stderr, "%s: exec %s: %s\n", prog, target[0], strerror(errno));
		_exit(127);
	}
	close(pfd[0]);

	struct perf_event_attr a;
	const char *evname = NULL;
	int fd = -1;

	/* Mechanism ladder. The hardware cycle counter is the best signal when a PMU
	 * is exposed; a VM or container without PMU pass-through has none, and then
	 * the software cpu-clock hrtimer is what you get. Whichever one opens is
	 * reported by name, so the output never claims a mechanism it did not get. */
	for (int attempt = 0; attempt < 3 && fd < 0; attempt++) {
		memset(&a, 0, sizeof a);
		a.size = sizeof a;
		switch (attempt) {
		case 0: a.type = PERF_TYPE_HARDWARE; a.config = PERF_COUNT_HW_CPU_CYCLES;
		        evname = "hw cpu-cycles"; break;
		case 1: a.type = PERF_TYPE_SOFTWARE; a.config = PERF_COUNT_SW_CPU_CLOCK;
		        evname = "sw cpu-clock"; break;
		default: a.type = PERF_TYPE_SOFTWARE; a.config = PERF_COUNT_SW_TASK_CLOCK;
		        evname = "sw task-clock"; break;
		}
		a.sample_freq = hz;
		a.freq = 1;
		a.disabled = 1;
		a.enable_on_exec = 1;
		a.exclude_kernel = 1; /* required at perf_event_paranoid >= 2, and we
		                       * cannot unwind kernel stacks anyway */
		a.exclude_hv = 1;
		a.inherit = 1; /* follow threads and forked children */
		a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;

		/* One per-task event following the target wherever it is scheduled. */
		fd = perf_open(&a, pid, -1);
	}

	if (fd < 0) {
		int e = errno;
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		fprintf(stderr,
		        "%s: perf_event_open failed: %s\n"
		        "%s: perf_event_paranoid=%s (sampling needs <= 2; this build only ever\n"
		        "%s:   asks for user-space events, exclude_kernel=1)\n"
		        "%s: perf_event_max_sample_rate=%s (-F %d must not exceed it)\n",
		        prog, strerror(e), prog,
		        sysctl_str("/proc/sys/kernel/perf_event_paranoid", 0), prog, prog,
		        sysctl_str("/proc/sys/kernel/perf_event_max_sample_rate", 1), hz);
		return 3;
	}

	/* Release the child; counting starts at its exec. */
	if (write(pfd[1], "g", 1) != 1) die("write: %s", strerror(errno));
	close(pfd[1]);

	int status = 0;
	if (waitpid(pid, &status, 0) < 0) die("waitpid: %s", strerror(errno));

	/* Always on stderr: never on stdout, because a profiler that pollutes the
	 * target's stdout is not transparent and cannot be dropped into a pipeline. */
	fprintf(stderr, "%s: armed %s @ %d Hz; no ring buffer yet, so the kernel has\n"
	                "%s: nowhere to put the samples and drops them\n", prog, evname, hz, prog);

	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}
