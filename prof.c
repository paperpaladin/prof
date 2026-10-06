/* prof - a sampling CPU profiler built directly on perf_event_open(2).
 *
 * Usage: prof [-F HZ] [-o FILE] -- ./prog args...
 *
 * What it does, end to end:
 *   fork/exec the target with the perf events already attached (enable_on_exec),
 *   take periodic samples of the instruction pointer, resolve each address
 *   against the ELF .symtab/.dynsym of whatever file is mapped there, and
 *   aggregate into a flat profile.
 *
 * No libelf, no libunwind, no libdw. C11 + Linux headers only.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <elf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
 * String-keyed counter table.
 *
 * Used for exactly two things: folded stacks -> count, and symbol name ->
 * {self, total}. Two use cases, so one shared open-addressed table and no
 * further abstraction.
 * ------------------------------------------------------------------ */

struct ent {
	char *key;
	uint64_t self;  /* samples whose innermost frame is this symbol */
};

struct tab {
	struct ent *e;
	size_t cap; /* power of two */
	size_t n;
};

static uint64_t fnv1a(const char *s) {
	uint64_t h = 0xcbf29ce484222325ULL;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static void tab_init(struct tab *t, size_t cap) {
	t->cap = cap;
	t->n = 0;
	t->e = calloc(cap, sizeof *t->e);
	if (!t->e) die("out of memory");
}

static void tab_grow(struct tab *t) {
	struct tab nt;
	tab_init(&nt, t->cap * 2);
	for (size_t i = 0; i < t->cap; i++) {
		if (!t->e[i].key) continue;
		size_t m = nt.cap - 1, j = fnv1a(t->e[i].key) & m;
		while (nt.e[j].key) j = (j + 1) & m;
		nt.e[j] = t->e[i]; /* moves the key allocation, no re-strdup */
		nt.n++;
	}
	free(t->e);
	*t = nt;
}

/* Find-or-insert. Returned pointer is stable until the next insert. */
static struct ent *tab_get(struct tab *t, const char *key) {
	if (t->n * 10 >= t->cap * 7) tab_grow(t);
	size_t m = t->cap - 1, i = fnv1a(key) & m;
	while (t->e[i].key) {
		if (strcmp(t->e[i].key, key) == 0) return &t->e[i];
		i = (i + 1) & m;
	}
	t->e[i].key = strdup(key);
	if (!t->e[i].key) die("out of memory");
	t->n++;
	return &t->e[i];
}

/* ------------------------------------------------------------------ *
 * Address space of the target: executable mappings + ELF symbols.
 * ------------------------------------------------------------------ */

struct map {
	uint64_t start, end, pgoff;
	char path[PATH_MAX];
};

struct sym {
	uint64_t addr, size;
	char *name;
};

static struct map maps[512];
static int nmaps;
static struct sym *syms;
static size_t nsyms, csyms;

static void sym_add(uint64_t addr, uint64_t size, const char *name) {
	if (nsyms == csyms) {
		csyms = csyms ? csyms * 2 : 4096;
		syms = realloc(syms, csyms * sizeof *syms);
		if (!syms) die("out of memory");
	}
	syms[nsyms].addr = addr;
	syms[nsyms].size = size;
	syms[nsyms].name = strdup(name);
	nsyms++;
}

static int sym_cmp(const void *a, const void *b) {
	const struct sym *x = a, *y = b;
	if (x->addr < y->addr) return -1;
	if (x->addr > y->addr) return 1;
	return 0;
}

/* Load bias for a file mapped at m: the value to add to an st_value from the
 * file to get a runtime address. For ET_EXEC this comes out 0. For ET_DYN
 * (PIE and shared libraries) we locate the PT_LOAD segment that this mapping
 * is a window onto and subtract its vaddr/offset skew -- doing it per segment
 * rather than assuming p_vaddr == p_offset keeps us correct under -z
 * separate-code, where a binary has several PT_LOADs. */
static uint64_t load_bias(const Elf64_Ehdr *eh, const void *base, const struct map *m) {
	if (eh->e_type == ET_EXEC) return 0;
	const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)base + eh->e_phoff);
	for (int i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
		if (m->pgoff >= ph[i].p_offset && m->pgoff < ph[i].p_offset + ph[i].p_filesz)
			return m->start - (ph[i].p_vaddr + (m->pgoff - ph[i].p_offset));
	}
	return m->start - m->pgoff; /* best effort */
}

/* Parse .symtab (preferred) and .dynsym out of one ELF file by hand. */
static void load_elf_syms(const struct map *m) {
	int fd = open(m->path, O_RDONLY);
	if (fd < 0) return;
	struct stat st;
	if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(Elf64_Ehdr)) {
		close(fd);
		return;
	}
	char *b = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (b == MAP_FAILED) return;

	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)b;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
	    eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > (uint64_t)st.st_size)
		goto out; /* shortcut: 64-bit little-endian ELF only; no 32-bit, no cross-endian. */

	uint64_t bias = load_bias(eh, b, m);
	const Elf64_Shdr *sh = (const Elf64_Shdr *)(b + eh->e_shoff);

	for (int i = 0; i < eh->e_shnum; i++) {
		if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
		if (sh[i].sh_entsize != sizeof(Elf64_Sym) || sh[i].sh_link >= eh->e_shnum) continue;
		const Elf64_Shdr *strs = &sh[sh[i].sh_link];
		if (sh[i].sh_offset + sh[i].sh_size > (uint64_t)st.st_size) continue;
		if (strs->sh_offset + strs->sh_size > (uint64_t)st.st_size) continue;

		const Elf64_Sym *sy = (const Elf64_Sym *)(b + sh[i].sh_offset);
		size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
		const char *sp = b + strs->sh_offset;
		for (size_t k = 0; k < n; k++) {
			if (ELF64_ST_TYPE(sy[k].st_info) != STT_FUNC) continue;
			if (sy[k].st_shndx == SHN_UNDEF || sy[k].st_value == 0) continue;
			if (sy[k].st_name >= strs->sh_size) continue;
			const char *nm = sp + sy[k].st_name;
			if (!*nm) continue;
			sym_add(bias + sy[k].st_value, sy[k].st_size, nm);
		}
		/* .symtab is a superset of .dynsym, so stop once we've found it. */
		if (sh[i].sh_type == SHT_SYMTAB) break;
	}
out:
	munmap(b, st.st_size);
}

static const char *base_name(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* Snapshot the target's executable mappings. Polls until the exec'd image
 * itself shows up, because we attach the perf events before exec() and would
 * otherwise race and snapshot our own (pre-exec) address space.
 * shortcut: one snapshot only. A library dlopen()ed later is invisible, and
 * the fix is to consume PERF_RECORD_MMAP2 records from the ring buffer. */
static int snapshot_maps(pid_t pid, const char *want) {
	char mp[64], want_real[PATH_MAX];
	snprintf(mp, sizeof mp, "/proc/%d/maps", (int)pid);
	if (!realpath(want, want_real)) want_real[0] = 0;

	for (int tries = 0; tries < 2000; tries++) {
		FILE *f = fopen(mp, "r");
		if (!f) return 0; /* already gone */
		char line[PATH_MAX + 128];
		int found = 0;
		nmaps = 0;
		while (fgets(line, sizeof line, f)) {
			uint64_t s, e, off;
			char perms[8], path[PATH_MAX];
			unsigned d1, d2;
			unsigned long ino;
			path[0] = 0;
			if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %" SCNx64 " %x:%x %lu %[^\n]",
			           &s, &e, perms, &off, &d1, &d2, &ino, path) < 7)
				continue;
			if (perms[2] != 'x') continue;
			char *p = path;
			while (*p == ' ') p++;
			if (!*p || *p == '[') continue; /* [vdso], [vsyscall]: nothing to parse */
			if (nmaps == (int)(sizeof maps / sizeof *maps)) break;
			maps[nmaps].start = s;
			maps[nmaps].end = e;
			maps[nmaps].pgoff = off;
			snprintf(maps[nmaps].path, PATH_MAX, "%s", p);
			nmaps++;
			if (want_real[0] && strcmp(p, want_real) == 0) found = 1;
		}
		fclose(f);
		if (found || !want_real[0]) break;
		usleep(500);
	}

	/* Parse each (path, bias) pair once. */
	for (int i = 0; i < nmaps; i++) {
		int dup = 0;
		for (int j = 0; j < i; j++)
			if (strcmp(maps[i].path, maps[j].path) == 0 &&
			    maps[i].pgoff - maps[i].start == maps[j].pgoff - maps[j].start)
				dup = 1;
		if (!dup) load_elf_syms(&maps[i]);
	}
	qsort(syms, nsyms, sizeof *syms, sym_cmp);

	return nmaps;
}

/* Greatest symbol whose address is <= a. Falls back to the module basename so
 * that an unresolved frame in a stripped library is still attributable. */
static const char *resolve(uint64_t a, char *buf, size_t bufsz) {
	size_t lo = 0, hi = nsyms, best = (size_t)-1;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (syms[mid].addr <= a) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	/* shortcut: symbols only, no JIT support. A JVM/V8/LuaJIT frame has no ELF
	 * symbol and falls through to module+offset below; reading
	 * /tmp/perf-PID.map is the established fix. */
	if (best != (size_t)-1) {
		/* st_size == 0 happens for hand-written asm; accept it rather than
		 * dropping the symbol, and let the next symbol bound it. */
		uint64_t end = syms[best].size ? syms[best].addr + syms[best].size
		                               : (best + 1 < nsyms ? syms[best + 1].addr : a + 1);
		if (a < end) return syms[best].name;
	}
	for (int i = 0; i < nmaps; i++)
		if (a >= maps[i].start && a < maps[i].end) {
			snprintf(buf, bufsz, "%s+0x%" PRIx64, base_name(maps[i].path), a - maps[i].start);
			return buf;
		}
	snprintf(buf, bufsz, "0x%" PRIx64, a);
	return buf;
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
static struct tab flat;

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

static void handle_sample(const unsigned char *p, size_t len);

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
			handle_sample(rec + sizeof h, h.size - sizeof h);
		else if (h.type == PERF_RECORD_LOST && h.size >= sizeof h + 16)
			n_lost += *(uint64_t *)(rec + sizeof h + 8);
	}

	__atomic_thread_fence(__ATOMIC_RELEASE); /* finish reading before publishing tail */
	r->meta->data_tail = tail;
}

/* ------------------------------------------------------------------ *
 * The unwinder.
 * ------------------------------------------------------------------ */

static uint64_t ld64(const unsigned char *p) {
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

/* Sample layout, in the fixed order documented in perf_event_open(2):
 *   u64 ip                                      (PERF_SAMPLE_IP)
 *   u32 pid, tid                                (PERF_SAMPLE_TID)
 * Only the innermost address for now; stacks come later.
 */
static void handle_sample(const unsigned char *p, size_t len) {
	if (len < 8 + 8) return; /* ip + pid/tid */
	uint64_t ip = ld64(p);
	n_samples++;

	char buf[64];
	tab_get(&flat, resolve(ip, buf, sizeof buf))->self++;
}

/* ------------------------------------------------------------------ *
 * Output.
 * ------------------------------------------------------------------ */

static int by_self(const void *a, const void *b) {
	const struct ent *x = a, *y = b;
	if (x->self != y->self) return x->self < y->self ? 1 : -1;
	return strcmp(x->key, y->key);
}

static struct ent *sorted(struct tab *t, size_t *n) {
	struct ent *v = xmalloc((t->n ? t->n : 1) * sizeof *v);
	size_t k = 0;
	for (size_t i = 0; i < t->cap; i++)
		if (t->e[i].key) v[k++] = t->e[i];
	qsort(v, k, sizeof *v, by_self);
	*n = k;
	return v;
}

static void emit_flat(FILE *o) {
	size_t n;
	struct ent *v = sorted(&flat, &n);
	fprintf(o, "# flat profile: %" PRIu64 " samples\n", n_samples);
	fprintf(o, "#   self%%     self  symbol\n");
	for (size_t i = 0; i < n; i++) {
		double s = n_samples ? 100.0 * v[i].self / n_samples : 0;
		fprintf(o, "  %6.2f %8" PRIu64 "  %s\n", s, v[i].self, v[i].key);
	}
	free(v);
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
	        "usage: %s [-F HZ] [-o FILE] -- ./prog [args...]\n"
	        "  -F HZ      sample frequency, default 997\n"
	        "  -o FILE    write the profile here (default: stderr)\n",
	        prog);
	exit(2);
}

int main(int argc, char **argv) {
	int hz = 997;
	const char *out = NULL;
	int i = 1;

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		else if (!strcmp(argv[i], "-F") && i + 1 < argc) hz = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
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

	tab_init(&flat, 1 << 10);

	/* Release the child, then learn its address space. */
	if (write(pfd[1], "g", 1) != 1) die("write: %s", strerror(errno));
	close(pfd[1]);
	snapshot_maps(pid, target[0]);

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

	FILE *o = stderr;
	if (out) {
		o = fopen(out, "w");
		if (!o) die("%s: %s", out, strerror(errno));
	}
	emit_flat(o);
	if (o != stderr) fclose(o);

	/* Always on stderr: never on stdout, because a profiler that pollutes the
	 * target's stdout is not transparent and cannot be dropped into a pipeline. */
	fprintf(stderr, "%s: %" PRIu64 " samples, %" PRIu64 " lost, %s @ %d Hz, %d/%d cpus\n",
	        prog, n_samples, n_lost, evname, hz, nrings, ncpu);
	if (n_samples == 0)
		fprintf(stderr, "%s: no samples -- target too short-lived, or off-CPU the whole time\n", prog);

	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}
