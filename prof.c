/* prof - a sampling CPU profiler built directly on perf_event_open(2).
 *
 * Usage: prof [--] ./prog args...
 *
 * What it does so far: runs the target and passes its exit status back. The
 * child blocks on a pipe until the parent says go, which is the hook the
 * sampling events will be attached through: they have to be armed before the
 * exec, or the first and most interesting milliseconds are unsampled.
 *
 * No libelf, no libunwind, no libdw. C11 + Linux headers only.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void usage(void) {
	fprintf(stderr,
	        "usage: %s [--] ./prog [args...]\n"
	        "runs ./prog unchanged and passes its exit status back\n",
	        prog);
	exit(2);
}

int main(int argc, char **argv) {
	int i = 1;

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) usage();
		else if (argv[i][0] == '-') die("unknown option %s (try -h)", argv[i]);
		else break; /* bare program name, no -- */
	}
	if (i >= argc) usage();
	char **target = argv + i;

	/* Hand the child a pipe to block on. Nothing needs it yet, but the perf
	 * events have to be attached between fork and exec, and this is where. */
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

	/* Release the child and get out of its way. Nothing is injected into it, no
	 * signal handler is installed in it, no environment variable is set for it,
	 * and its stdin/stdout/stderr are its own -- which is the hard invariant a
	 * profiler has to keep whatever else it does. */
	if (write(pfd[1], "g", 1) != 1) die("write: %s", strerror(errno));
	close(pfd[1]);

	int status = 0;
	if (waitpid(pid, &status, 0) < 0) die("waitpid: %s", strerror(errno));

	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}
