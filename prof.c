/* prof - a sampling CPU profiler built directly on perf_event_open(2).
 *
 * Usage: prof [-F HZ] [-o FILE] -- ./prog args...
 *
 * What it does, end to end:
 *   fork/exec the target with the perf events already attached (enable_on_exec),
 *   drain the per-CPU ring buffers without blocking, and report how many
 *   samples arrived and how many the kernel had to drop.
 *
 * No libelf, no libunwind, no libdw. C11 + Linux headers only.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* shortcut: a fixed ring size instead of a tuning knob. It must stay under
 * perf_event_mlock_kb per CPU. Upgrade path, if it ever bites: make it a flag. */
#define BUF_PAGES   64       /* data pages per per-CPU ring buffer (power of 2) */

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

static void *xmalloc(size_t n) {
	void *p = malloc(n);
	if (!p) die("out of memory");
	return p;
}

/* ------------------------------------------------------------------ *
 * perf_event_open plumbing.
 * ------------------------------------------------------------------ */

struct ring {
	int fd;
	struct perf_event_mmap_page *meta;
	unsigned char *data;
	size_t dsize;
};

static struct ring *rings;
static int nrings;
static uint64_t n_samples, n_lost;

static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu) {
	return syscall(SYS_perf_event_open, a, pid, cpu, -1, 0UL);
}

/* Copy n bytes out of the ring at logical offset off, unwrapping the circular
 * buffer. Records straddle the end of the data area, so nothing may read the
 * mapping linearly. */
static void ring_copy(struct ring *r, uint64_t off, void *dst, size_t n) {
	size_t m = r->dsize - 1, o = off & m;
	size_t first = r->dsize - o;
	if (first > n) first = n;
	memcpy(dst, r->data + o, first);
	if (n > first) memcpy((char *)dst + first, r->data, n - first);
}

/* Drain one per-CPU buffer. Must never block: while we are not draining the
 * kernel keeps writing, and once data_head laps data_tail the kernel drops
 * samples and reports PERF_RECORD_LOST. The whole loop is therefore
 * poll()-with-timeout plus waitpid(WNOHANG), never a blocking wait on the
 * child. */
static void drain(struct ring *r) {
	uint64_t head = r->meta->data_head;
	__atomic_thread_fence(__ATOMIC_ACQUIRE); /* pairs with the kernel's store-release of data_head */
	uint64_t tail = r->meta->data_tail;

	static unsigned char rec[1 << 16];
	while (tail < head) {
		struct perf_event_header h;
		ring_copy(r, tail, &h, sizeof h);
		/* h.size is a u16, so it always fits rec[]; a zero size would loop forever. */
		if (h.size < sizeof h) break;
		ring_copy(r, tail, rec, h.size);
		tail += h.size;
		if (h.type == PERF_RECORD_SAMPLE)
			n_samples++; /* contents are decoded in the next commit */
		else if (h.type == PERF_RECORD_LOST && h.size >= sizeof h + 16)
			n_lost += *(uint64_t *)(rec + sizeof h + 8);
	}

	__atomic_thread_fence(__ATOMIC_RELEASE); /* finish reading before publishing tail */
	r->meta->data_tail = tail;
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
	int ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) ncpu = 1;
	rings = xmalloc(ncpu * sizeof *rings);

	/* Prefer the hardware cycle counter; fall back to the software cpu-clock
	 * hrtimer, which is all you get in a VM or container with no PMU pass-through. */
	for (int attempt = 0; attempt < 3 && nrings == 0; attempt++) {
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

		/* One event per CPU rather than one per-task event: the kernel refuses
		 * to mmap a per-task event with inherit=1 (all children would share one
		 * ring), so following threads means binding per CPU, exactly as
		 * perf record does. */
		for (int c = 0; c < ncpu; c++) {
			long fd = perf_open(&a, pid, c);
			if (fd < 0) continue;
			size_t len = (BUF_PAGES + 1) * (size_t)sysconf(_SC_PAGESIZE);
			void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (m == MAP_FAILED) { close(fd); continue; }
			rings[nrings].fd = fd;
			rings[nrings].meta = m;
			rings[nrings].data = (unsigned char *)m + sysconf(_SC_PAGESIZE);
			rings[nrings].dsize = len - sysconf(_SC_PAGESIZE);
			nrings++;
		}
	}

	if (nrings == 0) {
		int e = errno;
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		fprintf(stderr,
		        "%s: perf_event_open failed on every CPU: %s\n"
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

	struct pollfd *pfds = xmalloc(nrings * sizeof *pfds);
	for (int c = 0; c < nrings; c++) {
		pfds[c].fd = rings[c].fd;
		pfds[c].events = POLLIN;
	}

	int status = 0, reaped = 0;
	while (!reaped) {
		poll(pfds, nrings, 20);
		for (int c = 0; c < nrings; c++) drain(&rings[c]);
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid) reaped = 1;
		else if (w < 0 && errno != EINTR) break;
	}
	/* The child is gone but its last samples are still in the rings. */
	for (int c = 0; c < nrings; c++) drain(&rings[c]);

	/* Always on stderr: never on stdout, because a profiler that pollutes the
	 * target's stdout is not transparent and cannot be dropped into a pipeline. */
	fprintf(stderr, "%s: %" PRIu64 " samples, %" PRIu64 " lost, %s @ %d Hz, %d/%d cpus\n",
	        prog, n_samples, n_lost, evname, hz, nrings, ncpu);
	if (n_samples == 0)
		fprintf(stderr, "%s: no samples -- target too short-lived, or off-CPU the whole time\n", prog);

	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}
