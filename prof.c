/* prof - a sampling CPU profiler built directly on perf_event_open(2).
 *
 * Usage: prof [-F HZ] [-o FILE] [--folded] -- ./prog args...
 *
 * What it does, end to end:
 *   fork/exec the target with the perf events already attached (enable_on_exec),
 *   take periodic samples of {RIP, RBP, RSP, top-of-stack bytes}, unwind the
 *   stack ourselves by walking the frame-pointer chain, resolve each address
 *   against the ELF .symtab/.dynsym of whatever file is mapped there, and
 *   aggregate into a flat profile plus collapsed ("folded") stacks.
 *
 * No libelf, no libunwind, no libdw. C11 + Linux headers only.
 *
 * Unwinding is DWARF .eh_frame CFI, with the saved-RBP chain as the fallback
 * for an object whose .eh_frame we cannot index. That order matters: frame
 * pointers are omitted by every -O2 build and by every distro libc, so an
 * RBP-only unwinder gives depth-1 stacks for anything it did not compile
 * itself. The summary line reports how many frames came from each, so a
 * profile that degraded says so.
 *
 * shortcut: a CFA given by a DWARF expression (DW_CFA_def_cfa_expression,
 * which gcc emits for the PLT and for a handful of libc's hand-written asm)
 * is not evaluated, and neither are DW_CFA_expression register rules; those
 * frames fall back to frame pointers. The upgrade path is a DWARF expression
 * evaluator over the captured registers and stack. Signal-trampoline frames
 * ('S' in a CIE augmentation) are also treated like ordinary ones.
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

/* shortcut: three fixed sizes instead of tuning knobs. MAX_DEPTH truncates very
 * deep stacks, STACK_BYTES bounds how far back we can walk (deeper frames are
 * counted in the "truncated stacks" figure rather than silently dropped), and
 * BUF_PAGES must stay under perf_event_mlock_kb per CPU. Upgrade path, if any
 * of them ever bites: make them flags. None has bitten yet. */
#define MAX_DEPTH   128      /* frames per stack; deeper is truncated */
#define STACK_BYTES 8192     /* user stack bytes copied per sample */
#define BUF_PAGES   64       /* data pages per per-CPU ring buffer (power of 2) */

static const char *prog = "prof";

/* --no-cfi: refuse to use .eh_frame and unwind with frame pointers only.
 * Not a tuning knob. It exists because "CFI recovered this call chain" is a
 * claim about CFI, and the only way for the suite to show it is a claim about
 * CFI rather than about the chain being easy is to turn CFI off and watch the
 * same binary collapse to depth 1. t/run.sh asserts both halves. */
static int no_cfi;

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
	uint64_t self;  /* folded table: sample count. sym table: leaf samples. */
	uint64_t total; /* sym table only: samples with this symbol anywhere.   */
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
	int obj; /* index into objs[], or -1 while unresolved */
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

/* ------------------------------------------------------------------ *
 * DWARF .eh_frame CFI: the part that reads the file.
 *
 * Frame pointers are optional on x86-64 and absent from every -O2 build, so
 * the saved-RBP walk further down is the fallback and this is the main
 * unwinder. The compiler is obliged to emit call-frame information for
 * exception handling even at -O2 -fomit-frame-pointer, and the linker builds
 * .eh_frame_hdr: a version byte, three encoding bytes, a pointer to
 * .eh_frame, a count, and then a table of (initial location, FDE pointer)
 * pairs SORTED by address. So resolving a sampled PC to its unwind rules is a
 * binary search, not a scan of every FDE.
 *
 * From the FDE we get a CIE (code/data alignment factors, the return-address
 * register, the pointer encoding) and a program of CFI opcodes. Running that
 * program up to the sampled PC yields one row: a rule for the canonical frame
 * address (CFA = some register + offset) and a rule per callee-saved
 * register. The caller's SP is the CFA by definition, and its return address
 * and RBP are usually stored at a fixed offset from it.
 *
 * The oracle for all of this is readelf --debug-dump=frames-interp, which
 * prints the same rows; prof --cfi-dump prints them in the same notation and
 * t/cfi_cmp.py compares them row by row. Nothing here is checked against
 * prof's own output.
 * ------------------------------------------------------------------ */

/* DWARF register numbers for x86-64 (System V psABI, figure 3.36). */
#define DW_RBP 6
#define DW_RSP 7
#define DW_RA  16
#define CFI_NREG 17 /* 0..16; only RBP, RSP and RA are ever consulted */

/* DW_EH_PE_* pointer encodings (LSB Core, "Exception Frames"). */
#define PE_OMIT     0xff
#define PE_FMT(e)   ((e) & 0x0f)
#define PE_APP(e)   ((e) & 0x70)
#define PE_INDIRECT 0x80

/* How a register's value in the caller's frame is recovered. The names match
 * DWARF 5 section 6.4.1; the notation in the comments is readelf's. */
enum {
	RU_UNDEF = 0,   /* "u"    not recoverable */
	RU_SAME,        /* "s"    unchanged by the callee */
	RU_OFFSET,      /* "c-8"  at *(CFA + v) */
	RU_VAL_OFFSET,  /* "v-8"  is CFA + v */
	RU_REG,         /* "r6"   in register v */
	RU_EXPR,        /* "exp"  at the address a DWARF expression computes */
	RU_VAL_EXPR     /* "vexp" is what a DWARF expression computes */
};

struct rule {
	unsigned char how;
	int64_t v;
};

struct row {
	int cfa_reg; /* -1 until a DW_CFA_def_cfa* names one */
	int cfa_exp; /* CFA is a DWARF expression: refused, not guessed at */
	int64_t cfa_off;
	struct rule r[CFI_NREG];
};

struct cfi {
	/* Exact-size copies of the two sections, NOT the mmap of the file.
	 *
	 * The file is mapped at st_size but a mapping is a whole number of
	 * pages, and the bytes between the end of the file and the end of the
	 * last page read as zero. A parser that walks one byte past the end of
	 * .eh_frame inside that mapping therefore reads a plausible 0 and no
	 * sanitizer says anything: the buffer is larger than its data, which is
	 * the defect this collection has found seven times. malloc'd to the
	 * exact byte count, ASan has a redzone to trip over, which
	 * t/run.sh's planted-overread case requires it to do. */
	unsigned char *hdr, *frame;
	size_t hdrlen, framelen;
	uint64_t hdr_va, frame_va; /* link-time addresses of those two buffers */
	size_t tab_off;            /* search table's offset within hdr */
	uint64_t ntab;             /* entries in it */
	unsigned char tab_enc;
	size_t tab_esz; /* bytes per encoded value, so 2*tab_esz per entry */
	int ok;
};

/* A bounds-checked cursor. Every read goes through rd_ok(), and once bad is
 * set every later read returns 0, so a truncated or hostile section produces
 * a refusal rather than a wrong answer: callers test r.bad once at the end. */
struct rdr {
	const unsigned char *p, *base, *end;
	uint64_t base_va;
	int bad;
};

static int rd_ok(struct rdr *r, uint64_t n) {
	if (r->bad || (uint64_t)(r->end - r->p) < n) {
		r->bad = 1;
		return 0;
	}
	return 1;
}

/* Link-time address of the byte the cursor is on: the base for PE_pcrel. */
static uint64_t rd_va(const struct rdr *r) {
	return r->base_va + (uint64_t)(r->p - r->base);
}

static uint8_t rd_u8(struct rdr *r) {
	if (!rd_ok(r, 1)) return 0;
	return *r->p++;
}

static uint64_t rd_fixed(struct rdr *r, size_t n) {
	uint64_t v = 0;
	if (!rd_ok(r, n)) return 0;
	memcpy(&v, r->p, n); /* little-endian only, as everywhere else here */
	r->p += n;
	return v;
}

static uint64_t rd_uleb(struct rdr *r) {
	uint64_t v = 0;
	int s = 0;
	for (;;) {
		uint8_t b = rd_u8(r);
		if (r->bad) return 0;
		if (s < 64) v |= (uint64_t)(b & 0x7f) << s;
		s += 7;
		if (!(b & 0x80)) break;
	}
	return v;
}

static int64_t rd_sleb(struct rdr *r) {
	int64_t v = 0;
	int s = 0;
	uint8_t b = 0;
	do {
		b = rd_u8(r);
		if (r->bad) return 0;
		if (s < 64) v |= (int64_t)(b & 0x7f) << s;
		s += 7;
	} while (b & 0x80);
	if (s < 64 && (b & 0x40)) v |= -((int64_t)1 << s);
	return v;
}

/* Bytes occupied by one PE-encoded value, or 0 if it is variable-length.
 * A table whose entries are variable-length cannot be binary-searched. */
static size_t enc_size(unsigned char enc) {
	switch (PE_FMT(enc)) {
	case 0x02: case 0x0a: return 2;
	case 0x03: case 0x0b: return 4;
	case 0x00: case 0x04: case 0x0c: return 8;
	default: return 0;
	}
}

/* One PE-encoded pointer. datarel is the base for DW_EH_PE_datarel, which is
 * the .eh_frame_hdr address. pcrel is relative to the byte being read, which
 * is why the cursor tracks a link-time address. */
static uint64_t rd_enc(struct rdr *r, unsigned char enc, uint64_t datarel) {
	uint64_t pos = rd_va(r), v = 0;
	if (enc == PE_OMIT) return 0;
	switch (PE_FMT(enc)) {
	case 0x00: v = rd_fixed(r, 8); break;
	case 0x01: v = rd_uleb(r); break;
	case 0x02: v = rd_fixed(r, 2); break;
	case 0x03: v = rd_fixed(r, 4); break;
	case 0x04: v = rd_fixed(r, 8); break;
	case 0x09: v = (uint64_t)rd_sleb(r); break;
	case 0x0a: v = (uint64_t)(int64_t)(int16_t)rd_fixed(r, 2); break;
	case 0x0b: v = (uint64_t)(int64_t)(int32_t)rd_fixed(r, 4); break;
	case 0x0c: v = rd_fixed(r, 8); break;
	default: r->bad = 1; return 0;
	}
	switch (PE_APP(enc)) {
	case 0x00: break;
	case 0x10: v += pos; break;     /* pcrel */
	case 0x30: v += datarel; break; /* datarel */
	default:
		/* textrel, funcrel and aligned: not emitted on x86-64 by gcc or
		 * clang, and guessing a base would name the wrong function. */
		r->bad = 1;
		return 0;
	}
	if (enc & PE_INDIRECT) r->bad = 1; /* needs the live GOT; refuse */
	return v;
}

/* Step over a PE-encoded value without needing to understand its base. Used
 * for the personality routine pointer, which is routinely PE_indirect. */
static void rd_skip_enc(struct rdr *r, unsigned char enc) {
	size_t n;
	if (enc == PE_OMIT) return;
	n = enc_size(enc);
	if (n) {
		if (rd_ok(r, n)) r->p += n;
	} else if (PE_FMT(enc) == 0x01) {
		rd_uleb(r);
	} else if (PE_FMT(enc) == 0x09) {
		rd_sleb(r);
	} else {
		r->bad = 1;
	}
}

/* mmap one ELF file and check the header enough to index into it. Returns the
 * mapping with *size set, or NULL. The caller munmaps. */
static char *elf_map(const char *path, size_t *size, const Elf64_Ehdr **ehp) {
	int fd = open(path, O_RDONLY);
	struct stat st;
	char *b;
	const Elf64_Ehdr *eh;
	if (fd < 0) return NULL;
	if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(Elf64_Ehdr)) {
		close(fd);
		return NULL;
	}
	b = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (b == MAP_FAILED) return NULL;
	eh = (const Elf64_Ehdr *)b;
	/* shortcut: 64-bit little-endian ELF only; no 32-bit, no cross-endian. */
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_phentsize != sizeof(Elf64_Phdr) ||
	    eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > (uint64_t)st.st_size) {
		munmap(b, st.st_size);
		return NULL;
	}
	*size = st.st_size;
	*ehp = eh;
	return b;
}

/* Read .eh_frame_hdr and .eh_frame out of a mapped ELF file into c. Returns 0
 * and leaves c->ok == 0 for any object we cannot index: no PT_GNU_EH_FRAME,
 * no search table, an encoding we do not implement. The caller then falls
 * back to frame pointers for that object rather than inventing frames. */
static int cfi_load(struct cfi *c, const char *b, size_t size, const Elf64_Ehdr *eh) {
	const Elf64_Phdr *ph = (const Elf64_Phdr *)(b + eh->e_phoff);
	uint64_t hva = 0, hoff = 0, hsz = 0, fva, n, fsz = 0, foff = 0;
	unsigned char fptr_enc, cnt_enc, tab_enc;
	size_t esz, toff;
	struct rdr r;
	int i;

	memset(c, 0, sizeof *c);
	for (i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_GNU_EH_FRAME) {
			hva = ph[i].p_vaddr;
			hoff = ph[i].p_offset;
			hsz = ph[i].p_filesz;
		}
	if (hsz < 4 || hoff + hsz > size) return 0;

	r.base = r.p = (const unsigned char *)b + hoff;
	r.end = r.base + hsz;
	r.base_va = hva;
	r.bad = 0;
	if (rd_u8(&r) != 1) return 0; /* version; only 1 has ever existed */
	fptr_enc = rd_u8(&r);
	cnt_enc = rd_u8(&r);
	tab_enc = rd_u8(&r);
	fva = rd_enc(&r, fptr_enc, hva);
	if (cnt_enc == PE_OMIT || tab_enc == PE_OMIT) return 0;
	n = rd_enc(&r, cnt_enc, hva);
	esz = enc_size(tab_enc);
	toff = (size_t)(r.p - r.base);
	if (r.bad || n == 0 || esz == 0) return 0;
	if (n > (hsz - toff) / (2 * esz)) return 0;

	/* .eh_frame's length is not in any program header, so take it from the
	 * section header when there is one (exact), and otherwise from the end
	 * of the containing PT_LOAD.
	 *
	 * shortcut: that fallback can include whatever follows .eh_frame in the
	 * same segment. It costs nothing in correctness because a walk only ever
	 * reaches those bytes through a length field in a CIE or FDE we chased
	 * there, and every read is bounds-checked; the upgrade path is to stop at
	 * the zero terminator the linker writes. Objects with section headers,
	 * which includes every stripped distro library, take the exact path. */
	if (eh->e_shoff && eh->e_shentsize == sizeof(Elf64_Shdr) &&
	    eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) <= size) {
		const Elf64_Shdr *sh = (const Elf64_Shdr *)(b + eh->e_shoff);
		for (i = 0; i < eh->e_shnum; i++)
			if (sh[i].sh_addr == fva && sh[i].sh_type == SHT_PROGBITS && sh[i].sh_size) {
				fsz = sh[i].sh_size;
				foff = sh[i].sh_offset;
			}
	}
	if (!fsz)
		for (i = 0; i < eh->e_phnum; i++)
			if (ph[i].p_type == PT_LOAD && fva >= ph[i].p_vaddr &&
			    fva < ph[i].p_vaddr + ph[i].p_filesz) {
				foff = ph[i].p_offset + (fva - ph[i].p_vaddr);
				fsz = ph[i].p_vaddr + ph[i].p_filesz - fva;
			}
	if (fsz < 4 || foff + fsz > size) return 0;

	c->hdr = xmalloc(hsz);
	memcpy(c->hdr, b + hoff, hsz);
	c->frame = xmalloc(fsz);
	memcpy(c->frame, b + foff, fsz);
	c->hdrlen = hsz;
	c->framelen = fsz;
	c->hdr_va = hva;
	c->frame_va = fva;
	c->tab_off = toff;
	c->ntab = n;
	c->tab_enc = tab_enc;
	c->tab_esz = esz;
	c->ok = 1;
#if PROF_PLANT_OVERREAD == 1
	/* A deliberate one-byte over-read, built only by the prof-asan-plant1
	 * target. The suite requires ASan to report it: that is the proof that
	 * this buffer is sized to its data, and so that a REAL overrun in the CFI
	 * parser would be reported too. With the sections left inside the file's
	 * mmap, or copied into a rounded-up buffer, the read below is silently
	 * fine and the sanitizer is off for everything in here. */
	fprintf(stderr, "%s: planted over-read: %02x\n", prog, c->frame[c->framelen]);
#endif
	return 1;
}

static void cfi_free(struct cfi *c) {
	free(c->hdr);
	free(c->frame);
	memset(c, 0, sizeof *c);
}

/* Binary search .eh_frame_hdr's sorted table for the last entry whose initial
 * location is <= va, and return the FDE address it points at (0 if none).
 * This is the whole reason .eh_frame_hdr exists: libc's .eh_frame holds
 * around 18000 FDEs and a sampling profiler cannot scan them per sample. */
static uint64_t cfi_lookup(const struct cfi *c, uint64_t va) {
	size_t lo = 0, hi = (size_t)c->ntab, best = (size_t)-1, step = 2 * c->tab_esz;
	struct rdr r;
	uint64_t loc, fde;

	r.base = c->hdr;
	r.end = c->hdr + c->hdrlen;
	r.base_va = c->hdr_va;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		r.p = c->hdr + c->tab_off + mid * step;
		r.bad = 0;
		loc = rd_enc(&r, c->tab_enc, c->hdr_va);
		if (r.bad) return 0;
		if (loc <= va) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (best == (size_t)-1) return 0;
	r.p = c->hdr + c->tab_off + best * step;
	r.bad = 0;
	rd_enc(&r, c->tab_enc, c->hdr_va);
	fde = rd_enc(&r, c->tab_enc, c->hdr_va);
	return r.bad ? 0 : fde;
}

struct cie {
	uint64_t code_align;
	int64_t data_align;
	int ra_reg;
	int has_z, sig;
	unsigned char fde_enc;
	const unsigned char *insn, *insn_end;
};

/* Point a cursor at a link-time address inside .eh_frame. */
static int cfi_at(struct rdr *r, const struct cfi *c, uint64_t va) {
	if (va < c->frame_va || va - c->frame_va >= c->framelen) return 0;
	r->base = c->frame;
	r->end = c->frame + c->framelen;
	r->base_va = c->frame_va;
	r->p = c->frame + (va - c->frame_va);
	r->bad = 0;
	return 1;
}

static int cie_parse(const struct cfi *c, uint64_t cva, struct cie *o) {
	struct rdr r;
	const unsigned char *end;
	char aug[16];
	size_t na = 0, i;
	uint64_t len;
	uint8_t ver;

	if (!cfi_at(&r, c, cva)) return 0;
	len = rd_fixed(&r, 4);
	/* shortcut: 32-bit DWARF only. A 0xffffffff length introduces the 64-bit
	 * form, which no x86-64 toolchain emits for .eh_frame; we refuse rather
	 * than misread the fields that follow. */
	if (len == 0 || len == 0xffffffffu || !rd_ok(&r, len)) return 0;
	end = r.p + len;
	if (rd_fixed(&r, 4) != 0) return 0; /* CIE id; nonzero means this is an FDE */
	ver = rd_u8(&r);
	if (ver != 1 && ver != 3) return 0;
	for (;;) {
		uint8_t ch = rd_u8(&r);
		if (r.bad) return 0;
		if (!ch) break;
		if (na < sizeof aug - 1) aug[na++] = (char)ch;
	}
	aug[na] = 0;
	/* An augmentation that does not start with 'z' carries no length, so
	 * anything we do not recognise would leave the cursor in the wrong
	 * place. Refuse the object instead. */
	if (na && aug[0] != 'z') return 0;
	o->code_align = rd_uleb(&r);
	o->data_align = rd_sleb(&r);
	o->ra_reg = ver == 1 ? (int)rd_u8(&r) : (int)rd_uleb(&r);
	o->fde_enc = 0; /* absptr unless 'R' says otherwise */
	o->sig = 0;
	o->has_z = na > 0;
	if (o->has_z) {
		uint64_t alen = rd_uleb(&r);
		const unsigned char *aend;
		if (!rd_ok(&r, alen)) return 0;
		aend = r.p + alen;
		for (i = 1; i < na; i++) {
			switch (aug[i]) {
			case 'R': o->fde_enc = rd_u8(&r); break;
			case 'P': rd_skip_enc(&r, rd_u8(&r)); break; /* personality */
			case 'L': rd_u8(&r); break;                  /* LSDA encoding */
			case 'S': o->sig = 1; break;
			default: return 0;
			}
			if (r.bad) return 0;
		}
		r.p = aend; /* the length is authoritative over our walk of it */
	}
	if (r.bad || o->code_align == 0 || o->ra_reg >= CFI_NREG) return 0;
	o->insn = r.p;
	o->insn_end = end;
	return 1;
}

static void setrule(struct row *w, uint64_t reg, int how, int64_t v) {
	if (reg >= CFI_NREG) return; /* a register we never read; ignore the rule */
	w->r[reg].how = (unsigned char)how;
	w->r[reg].v = v;
}

/* Run a CFI opcode program, stopping at the first row whose location is past
 * va. DWARF 5 section 6.4.2; the opcode numbering is stable back to DWARF 2. */
static void cfi_exec(struct rdr *r, const struct cie *ci, uint64_t va,
                     uint64_t *loc, struct row *cur, const struct row *init) {
	struct row saved[8];
	int depth = 0;

	while (!r->bad && r->p < r->end) {
		uint8_t op = rd_u8(r);
		uint64_t adv, reg, u;
		int64_t s;
		if (r->bad) return;
		switch (op >> 6) {
		case 1: /* DW_CFA_advance_loc */
			adv = op & 0x3f;
			break;
		case 2: /* DW_CFA_offset */
			u = rd_uleb(r);
			setrule(cur, op & 0x3f, RU_OFFSET, (int64_t)u * ci->data_align);
			continue;
		case 3: /* DW_CFA_restore */
			if ((op & 0x3f) < CFI_NREG) cur->r[op & 0x3f] = init->r[op & 0x3f];
			continue;
		default:
			switch (op) {
			case 0x00: continue; /* nop */
			case 0x01:           /* set_loc */
				u = rd_enc(r, ci->fde_enc, 0);
				if (r->bad || u > va) return;
				*loc = u;
				continue;
			case 0x02: adv = rd_u8(r); break;
			case 0x03: adv = rd_fixed(r, 2); break;
			case 0x04: adv = rd_fixed(r, 4); break;
			case 0x05: /* offset_extended */
				reg = rd_uleb(r);
				u = rd_uleb(r);
				setrule(cur, reg, RU_OFFSET, (int64_t)u * ci->data_align);
				continue;
			case 0x06: /* restore_extended */
				reg = rd_uleb(r);
				if (reg < CFI_NREG) cur->r[reg] = init->r[reg];
				continue;
			case 0x07: setrule(cur, rd_uleb(r), RU_UNDEF, 0); continue;
			case 0x08: setrule(cur, rd_uleb(r), RU_SAME, 0); continue;
			case 0x09: /* register */
				reg = rd_uleb(r);
				u = rd_uleb(r);
				setrule(cur, reg, RU_REG, (int64_t)u);
				continue;
			case 0x0a: /* remember_state */
				if (depth < 8) saved[depth] = *cur;
				depth++;
				continue;
			case 0x0b: /* restore_state */
				if (depth > 0 && --depth < 8) *cur = saved[depth];
				continue;
			case 0x0c: /* def_cfa */
				cur->cfa_reg = (int)rd_uleb(r);
				cur->cfa_off = (int64_t)rd_uleb(r);
				cur->cfa_exp = 0;
				continue;
			case 0x0d: /* def_cfa_register */
				cur->cfa_reg = (int)rd_uleb(r);
				cur->cfa_exp = 0;
				continue;
			case 0x0e: cur->cfa_off = (int64_t)rd_uleb(r); continue;
			case 0x0f: /* def_cfa_expression */
				u = rd_uleb(r);
				cur->cfa_exp = 1;
				if (rd_ok(r, u)) r->p += u;
				continue;
			case 0x10: /* expression */
				reg = rd_uleb(r);
				u = rd_uleb(r);
				setrule(cur, reg, RU_EXPR, 0);
				if (rd_ok(r, u)) r->p += u;
				continue;
			case 0x11: /* offset_extended_sf */
				reg = rd_uleb(r);
				s = rd_sleb(r);
				setrule(cur, reg, RU_OFFSET, s * ci->data_align);
				continue;
			case 0x12: /* def_cfa_sf: offset is factored */
				cur->cfa_reg = (int)rd_uleb(r);
				cur->cfa_off = rd_sleb(r) * ci->data_align;
				cur->cfa_exp = 0;
				continue;
			case 0x13: cur->cfa_off = rd_sleb(r) * ci->data_align; continue;
			case 0x14: /* val_offset */
				reg = rd_uleb(r);
				u = rd_uleb(r);
				setrule(cur, reg, RU_VAL_OFFSET, (int64_t)u * ci->data_align);
				continue;
			case 0x15: /* val_offset_sf */
				reg = rd_uleb(r);
				s = rd_sleb(r);
				setrule(cur, reg, RU_VAL_OFFSET, s * ci->data_align);
				continue;
			case 0x16: /* val_expression */
				reg = rd_uleb(r);
				u = rd_uleb(r);
				setrule(cur, reg, RU_VAL_EXPR, 0);
				if (rd_ok(r, u)) r->p += u;
				continue;
			case 0x2d: continue;          /* GNU_window_save (sparc) */
			case 0x2e: rd_uleb(r); continue; /* GNU_args_size */
			case 0x2f:                    /* GNU_negative_offset_extended */
				reg = rd_uleb(r);
				u = rd_uleb(r);
				setrule(cur, reg, RU_OFFSET, -(int64_t)u * ci->data_align);
				continue;
			default:
				r->bad = 1; /* unknown opcode: stop, do not guess */
				return;
			}
		}
		/* An advance ends the current row. Stop before stepping past va, so
		 * the row left in *cur is the one that covers it. */
		if (r->bad) return;
		if (*loc + adv * ci->code_align > va) return;
		*loc += adv * ci->code_align;
	}
}

/* The unwind row for link-time address va, or 0 if this object has no CFI
 * covering it. va is a link-time address: the caller subtracts the load bias. */
static int cfi_row(const struct cfi *c, uint64_t va, struct row *out) {
	struct rdr f;
	struct cie ci;
	struct row cur, init;
	const unsigned char *fend;
	uint64_t len, cie_field_va, back, pc0, range, loc;

	if (!c->ok) return 0;
	if (!cfi_at(&f, c, cfi_lookup(c, va))) return 0;
	len = rd_fixed(&f, 4);
	if (len == 0 || len == 0xffffffffu || !rd_ok(&f, len)) return 0;
	fend = f.p + len;
	cie_field_va = rd_va(&f);
	back = rd_fixed(&f, 4);
	/* In .eh_frame (unlike .debug_frame) the CIE pointer is the distance
	 * backwards from the pointer field itself, and zero means "this is a
	 * CIE", which the search table should never point at. */
	if (f.bad || back == 0 || back > cie_field_va - c->frame_va) return 0;
	if (!cie_parse(c, cie_field_va - back, &ci)) return 0;
	pc0 = rd_enc(&f, ci.fde_enc, c->hdr_va);
	range = rd_enc(&f, PE_FMT(ci.fde_enc), 0); /* a length, never relocated */
	if (f.bad || va < pc0 || va - pc0 >= range) return 0;
	if (ci.has_z) {
		uint64_t al = rd_uleb(&f);
		if (!rd_ok(&f, al)) return 0;
		f.p += al;
	}

	memset(&cur, 0, sizeof cur);
	cur.cfa_reg = -1;
	f.end = fend;

	/* The CIE's instructions build the initial row, which is also what
	 * DW_CFA_restore restores to. */
	{
		struct rdr k = f;
		k.p = ci.insn;
		k.end = ci.insn_end;
		k.bad = 0;
		loc = 0;
		cfi_exec(&k, &ci, UINT64_MAX, &loc, &cur, &cur);
		if (k.bad) return 0;
	}
	init = cur;
	loc = pc0;
	cfi_exec(&f, &ci, va, &loc, &cur, &init);
	if (f.bad) return 0;
	*out = cur;
	return 1;
}

/* readelf --debug-dump=frames-interp's notation, so the two can be diffed. */
static const char *dwreg_name(int r) {
	static const char *n[] = {"rax", "rdx", "rcx", "rbx", "rsi", "rdi", "rbp",
	                          "rsp", "r8", "r9", "r10", "r11", "r12", "r13",
	                          "r14", "r15", "ra"};
	return (r >= 0 && r < CFI_NREG) ? n[r] : "r?";
}

static void cfa_str(const struct row *w, char *b, size_t n) {
	if (w->cfa_exp) snprintf(b, n, "exp");
	else if (w->cfa_reg < 0) snprintf(b, n, "u");
	else snprintf(b, n, "%s%+" PRId64, dwreg_name(w->cfa_reg), w->cfa_off);
}

static void rule_str(const struct rule *r, char *b, size_t n) {
	switch (r->how) {
	case RU_SAME: snprintf(b, n, "s"); break;
	case RU_OFFSET: snprintf(b, n, "c%+" PRId64, r->v); break;
	case RU_VAL_OFFSET: snprintf(b, n, "v%+" PRId64, r->v); break;
	case RU_REG: snprintf(b, n, "r%" PRId64, r->v); break;
	case RU_EXPR: snprintf(b, n, "exp"); break;
	case RU_VAL_EXPR: snprintf(b, n, "vexp"); break;
	default: snprintf(b, n, "u"); break;
	}
}

/* --cfi-dump FILE: read hex addresses on stdin, print one row each, in
 * readelf's notation. This exists so the CFI reader can be compared against
 * an external authority directly, address by address, without a running
 * target in the way; t/cfi_cmp.py drives it. */
static int cfi_dump(const char *path) {
	struct cfi c;
	size_t size;
	const Elf64_Ehdr *eh;
	char *b = elf_map(path, &size, &eh), line[64];
	int got;

	if (!b) die("%s: not a 64-bit little-endian ELF file", path);
	got = cfi_load(&c, b, size, eh);
	munmap(b, size);
	if (!got) die("%s: no usable .eh_frame_hdr search table", path);

	/* One line per address: the address, the CFA rule, then the rule for
	 * every register from 0 (rax) to 16 (ra) in DWARF numbering, which is the
	 * order dwreg_name() lists. A lone "-" means no FDE covers the address.
	 * All 17 are printed rather than just the two the unwinder reads, because
	 * readelf prints a column for every register an FDE mentions and there is
	 * no reason to compare fewer of them than the oracle offers. */
	while (fgets(line, sizeof line, stdin)) {
		uint64_t va = strtoull(line, NULL, 16);
		struct row w;
		char s[32];
		if (!cfi_row(&c, va, &w)) {
			printf("%016" PRIx64 " -\n", va);
			continue;
		}
		cfa_str(&w, s, sizeof s);
		printf("%016" PRIx64 " %s", va, s);
		for (int k = 0; k < CFI_NREG; k++) {
			rule_str(&w.r[k], s, sizeof s);
			printf(" %s", s);
		}
		printf("\n");
	}
	cfi_free(&c);
	return 0;
}

/* Parse .symtab (preferred), .dynsym and .eh_frame out of one ELF file by
 * hand, from a single mapping of it. Returns the load bias; *c is filled with
 * the object's CFI, with c->ok == 0 if it has none we can index. A file with
 * no section headers at all still has CFI: that is the stripped-distro-library
 * case, where the symbols are gone and the unwind tables are not. */
static uint64_t load_object(const struct map *m, struct cfi *c) {
	size_t size;
	const Elf64_Ehdr *eh;
	char *b = elf_map(m->path, &size, &eh);
	if (!b) {
		memset(c, 0, sizeof *c);
		return 0;
	}
	uint64_t bias = load_bias(eh, b, m);
	cfi_load(c, b, size, eh);

	if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
	    eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > (uint64_t)size)
		goto out;

	const Elf64_Shdr *sh = (const Elf64_Shdr *)(b + eh->e_shoff);

	for (int i = 0; i < eh->e_shnum; i++) {
		if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
		if (sh[i].sh_entsize != sizeof(Elf64_Sym) || sh[i].sh_link >= eh->e_shnum) continue;
		const Elf64_Shdr *strs = &sh[sh[i].sh_link];
		if (sh[i].sh_offset + sh[i].sh_size > (uint64_t)size) continue;
		if (strs->sh_offset + strs->sh_size > (uint64_t)size) continue;

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
	munmap(b, size);
	return bias;
}

static const char *base_name(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* shortcut: one flat symbol table shared by every sampled process. With
 * inherit=1 we also sample forked children, whose PIE addresses are resolved
 * against the parent's mappings and so can be named wrongly. Upgrade path:
 * key maps and symbols by pid, which needs PERF_RECORD_FORK/MMAP2 from the ring
 * buffer (Milestone 1). Fine for the single-process case this is built for.
 *
 * Objects whose symbols we have already pulled in, keyed by path + load skew,
 * so that re-reading /proc/PID/maps is cheap and idempotent. */
struct obj {
	char path[PATH_MAX];
	uint64_t skew; /* start - pgoff: identifies this mapping of this file */
	uint64_t bias; /* runtime address = link-time address + bias */
	struct cfi cfi;
};
static struct obj objs[512];
static int nobjs;

/* Re-read the target's executable mappings and pull in symbols for any object
 * we have not seen before.
 *
 * This is called repeatedly while the child runs rather than once at startup,
 * for two reasons. First, we attach the perf events before exec(), so a single
 * early snapshot races ld.so: we would see the main binary but not yet libc,
 * and every return address into libc would then look like it was outside any
 * executable mapping and silently truncate the stack. Second, it is the only
 * way to see a library that arrives later via dlopen(). It has to happen while
 * the process is alive, because /proc/PID/maps vanishes the moment it exits --
 * which is also why the final ring-buffer drain happens after the last refresh
 * rather than needing a fresh one. */
static int refresh_maps(pid_t pid) {
	char mp[64];
	snprintf(mp, sizeof mp, "/proc/%d/maps", (int)pid);
	FILE *f = fopen(mp, "r");
	if (!f) return 0;

	char line[PATH_MAX + 128];
	int n = 0, added = 0;
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
		if (n == (int)(sizeof maps / sizeof *maps)) break;
		maps[n].start = s;
		maps[n].end = e;
		maps[n].pgoff = off;
		maps[n].obj = -1;
		snprintf(maps[n].path, PATH_MAX, "%s", p);
		n++;
	}
	fclose(f);
	if (n == 0) return 0;
	nmaps = n;

	for (int i = 0; i < nmaps; i++) {
		uint64_t skew = maps[i].start - maps[i].pgoff;
		for (int j = 0; j < nobjs; j++)
			if (objs[j].skew == skew && strcmp(objs[j].path, maps[i].path) == 0)
				maps[i].obj = j;
		if (maps[i].obj >= 0) continue;
		if (nobjs == (int)(sizeof objs / sizeof *objs)) continue;
		struct obj *o = &objs[nobjs];
		snprintf(o->path, PATH_MAX, "%s", maps[i].path);
		o->skew = skew;
		maps[i].obj = nobjs++;
		o->bias = load_object(&maps[i], &o->cfi);
		added = 1;
	}
	if (added) qsort(syms, nsyms, sizeof *syms, sym_cmp);
	return nmaps;
}

/* Wait for the exec'd image itself to show up before the first drain, so the
 * earliest samples are resolvable. Bounded: a target that dies instantly just
 * yields an empty profile rather than hanging us. */
static void await_exec(pid_t pid, const char *want) {
	char real[PATH_MAX];
	if (!realpath(want, real)) return;
	for (int t = 0; t < 2000; t++) {
		if (refresh_maps(pid) == 0) return; /* process gone */
		for (int i = 0; i < nmaps; i++)
			if (strcmp(maps[i].path, real) == 0) return;
		usleep(500);
	}
}

static int in_text(uint64_t a) {
	for (int i = 0; i < nmaps; i++)
		if (a >= maps[i].start && a < maps[i].end) return 1;
	return 0;
}

/* The CFI of whatever object is mapped over a, with its load bias, or NULL.
 * This is the per-mapping unwinder choice: an object whose .eh_frame we could
 * index unwinds with CFI, and one we could not falls back to frame pointers,
 * which the summary line reports as a frame count per method rather than
 * leaving the reader to guess which they got. */
static const struct cfi *cfi_for(uint64_t a, uint64_t *bias) {
	for (int i = 0; i < nmaps; i++)
		if (a >= maps[i].start && a < maps[i].end && maps[i].obj >= 0) {
			*bias = objs[maps[i].obj].bias;
			return &objs[maps[i].obj].cfi;
		}
	return NULL;
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

/* sample_regs_user bitmask. The kernel delivers the selected registers in
 * ascending bit order, so regs[0]=RBP, regs[1]=RSP, regs[2]=RIP. */
#define REG_BP 6
#define REG_SP 7
#define REG_IP 8
#define REGMASK ((1ULL << REG_BP) | (1ULL << REG_SP) | (1ULL << REG_IP))

struct ring {
	int fd;
	struct perf_event_mmap_page *meta;
	unsigned char *data;
	size_t dsize;
};

static struct ring *rings;
static int nrings;
static uint64_t n_samples, n_lost, n_trunc;
static uint64_t n_cfi_frames, n_fp_frames, n_fp_samples;
static struct tab folded, flat;

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

	/* Sized to exactly h.size, not left at the u16 maximum.
	 *
	 * This buffer used to be a fixed 64 KB array, which meant a parse bug
	 * reading one byte past the end of a 4 KB sample landed 60 KB inside a
	 * live object and no sanitizer could possibly see it: a buffer larger
	 * than its data turns ASan off for everything inside it. Every sample in
	 * a run is the same size, so the realloc happens once. t/run.sh plants
	 * exactly that over-read and requires ASan to report it, so going back to
	 * a rounded-up buffer turns the suite red rather than quietly turning the
	 * sanitizer off. */
	static unsigned char *rec;
	static size_t reccap;
	while (tail < head) {
		struct perf_event_header h;
		ring_copy(r, tail, &h, sizeof h);
		/* A zero size would loop forever. */
		if (h.size < sizeof h) break;
		if (h.size != reccap) {
			unsigned char *nr = realloc(rec, h.size);
			if (!nr) die("out of memory");
			rec = nr;
			reccap = h.size;
		}
		ring_copy(r, tail, rec, h.size);
#if PROF_PLANT_OVERREAD == 2
		/* The same planted over-read for the record buffer; see plant 1. */
		fprintf(stderr, "%s: planted over-read: %02x\n", prog, rec[h.size]);
#endif
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

/* Read 8 bytes of the captured stack. The kernel copies len bytes starting at
 * the sampled RSP; an address outside that window was never captured, so the
 * walk stops there and is counted as a truncation rather than guessed at. */
static int stk_read(const unsigned char *s, uint64_t base, uint64_t len,
                    uint64_t a, uint64_t *v) {
	if (a < base || a - base + 8 > len) return 0;
	*v = ld64(s + (a - base));
	return 1;
}

enum { UNW_OK, UNW_NOCFI, UNW_STOP };

/* One CFI unwind step. pc is the address to look up: the sampled RIP for the
 * innermost frame, and (return address - 1) above it, because a return address
 * points after the call and can even be one past the end of the function.
 *
 * On UNW_OK, *ra is the caller's return address and sp/bp have been moved into
 * the caller's frame. UNW_NOCFI means this object has no CFI covering pc, or
 * has a rule this code does not evaluate, so the caller should try frame
 * pointers. UNW_STOP means the chain genuinely ends here: an undefined
 * return-address rule is how the CIE of a thread entry point says "outermost",
 * and a read outside the captured stack means the rest was never sampled. */
static int cfi_step(uint64_t pc, uint64_t *sp, uint64_t *bp, uint64_t *ra,
                    const unsigned char *stk, uint64_t base, uint64_t len) {
	uint64_t bias = 0, cfa, v;
	const struct cfi *c = cfi_for(pc, &bias);
	struct row w;

	if (no_cfi || !c || !c->ok || !cfi_row(c, pc - bias, &w)) return UNW_NOCFI;
	if (w.cfa_exp) return UNW_NOCFI; /* a DWARF expression CFA: not evaluated */
	if (w.cfa_reg == DW_RSP) cfa = *sp + (uint64_t)w.cfa_off;
	else if (w.cfa_reg == DW_RBP) cfa = *bp + (uint64_t)w.cfa_off;
	else return UNW_NOCFI; /* only three registers were sampled */

	switch (w.r[DW_RA].how) {
	case RU_OFFSET:
		if (!stk_read(stk, base, len, cfa + (uint64_t)w.r[DW_RA].v, ra))
			return UNW_STOP;
		break;
	case RU_UNDEF:
	case RU_SAME:
		return UNW_STOP;
	default:
		return UNW_NOCFI;
	}
	/* RBP only matters for the next step, and only if that frame's CFA is
	 * RBP-relative, but it is two lines to keep right. No rule for a
	 * callee-saved register means the callee left it alone. */
	if (w.r[DW_RBP].how == RU_OFFSET) {
		if (stk_read(stk, base, len, cfa + (uint64_t)w.r[DW_RBP].v, &v)) *bp = v;
	} else if (w.r[DW_RBP].how == RU_VAL_OFFSET) {
		*bp = cfa + (uint64_t)w.r[DW_RBP].v;
	}
	*sp = cfa; /* the CFA is the caller's stack pointer, by definition */
	return UNW_OK;
}

/* Sample layout, in the fixed order documented in perf_event_open(2):
 *   u64 ip                                      (PERF_SAMPLE_IP)
 *   u32 pid, tid                                (PERF_SAMPLE_TID)
 *   u64 abi; u64 regs[popcount(regmask)]        (PERF_SAMPLE_REGS_USER)
 *   u64 size; u8 data[size]; u64 dyn_size       (PERF_SAMPLE_STACK_USER)
 */
static void handle_sample(const unsigned char *p, size_t len) {
	const unsigned char *end = p + len;
	if (p + 8 + 8 > end) return;
	uint64_t ip = ld64(p);
	p += 8 + 8; /* ip, then pid+tid */

	if (p + 8 > end) return;
	uint64_t abi = ld64(p);
	p += 8;
	if (abi == PERF_SAMPLE_REGS_ABI_NONE) return; /* sampled in the kernel; we asked not to be */
	if (p + 3 * 8 > end) return;
	uint64_t bp = ld64(p), sp = ld64(p + 8), rip = ld64(p + 16);
	p += 3 * 8;
	if (rip) ip = rip;

	if (p + 8 > end) return;
	uint64_t ssize = ld64(p);
	p += 8;
	/* ssize bytes of stack, then the u64 dyn_size that says how many of them
	 * the kernel really filled. The +8 is the point: checking only ssize let
	 * the dyn_size read run 8 bytes past the record, which was invisible while
	 * the record buffer was a fixed 64 KB array. */
	if (ssize + 8 > (uint64_t)(end - p)) return;
	const unsigned char *stack = p;
	uint64_t dyn = ssize >= 8 ? ld64(p + ssize) : 0; /* bytes actually captured */
	if (dyn > ssize) dyn = ssize;

	n_samples++;

	/* The walk. CFI first for every frame, because frame pointers are absent
	 * from every -O2 build and from the whole of a distro libc; the saved-RBP
	 * chain is the fallback for an object whose .eh_frame we could not index.
	 * Which one produced each frame is counted and reported, so a profile that
	 * silently degraded is visible in the output instead of being inferred.
	 *
	 * Leaf frame first; reversed below for output. */
	uint64_t fr[MAX_DEPTH];
	uint64_t sp0 = sp; /* the stack window starts at the sampled RSP */
	int nf = 0, fell_back = 0;
	fr[nf++] = ip;
	uint64_t pc = ip;
	for (int d = 0; d < MAX_DEPTH - 1; d++) {
		uint64_t ra = 0;
		int last = 0;
		int st = cfi_step(pc, &sp, &bp, &ra, stack, sp0, dyn);
		if (st == UNW_STOP) break;
		if (st == UNW_NOCFI) {
			/* Frame-pointer fallback: the saved RBP and the return address
			 * sit at [rbp] and [rbp+8] once the prologue has run. */
			if (bp < sp0 || bp - sp0 + 16 > dyn) {
				if (bp >= sp0) n_trunc++; /* past the bytes the kernel copied */
				break;
			}
			uint64_t next = ld64(stack + (bp - sp0));
			ra = ld64(stack + (bp - sp0) + 8);
			/* The chain must grow toward the stack base. When it does not we
			 * have reached the outermost frame that has one, so this return
			 * address is still good and only the step after it is not. */
			if (next <= bp) last = 1;
			sp = bp + 16;
			bp = next;
		}
		/* A return address must land in a mapped executable region. On a frame
		 * built without frame pointers RBP holds live data instead of a saved
		 * RBP, and this is what stops the fallback walking nonsense. */
		if (!ra || !in_text(ra)) break;
		if (st == UNW_OK) {
			n_cfi_frames++;
		} else {
			n_fp_frames++;
			fell_back = 1;
		}
		/* ra points *after* the call instruction; step back so the address
		 * attributes to the call site and not to whatever follows it. */
		pc = ra - 1;
		fr[nf++] = pc;
		if (last) break;
	}
	if (fell_back) n_fp_samples++;

	/* Resolve, build the folded key outermost-first, and tally the flat table. */
	const char *names[MAX_DEPTH];
	char bufs[MAX_DEPTH][64];
	for (int i = 0; i < nf; i++) names[i] = resolve(fr[i], bufs[i], sizeof bufs[i]);

	/* shortcut: stacks are keyed by their rendered string rather than by an
	 * interned frame-id vector, so a stack longer than 4096 characters is
	 * truncated and merges with its prefix. Upgrade path is a trie keyed on
	 * symbol ids; the string is what flamegraph tools want anyway. */
	char key[4096];
	size_t kl = 0;
	for (int i = nf - 1; i >= 0; i--) {
		int w = snprintf(key + kl, sizeof key - kl, "%s%s", kl ? ";" : "", names[i]);
		if (w < 0 || kl + w >= sizeof key) break;
		kl += w;
	}
	tab_get(&folded, key)->self++;

	tab_get(&flat, names[0])->self++;
	for (int i = 0; i < nf; i++) {
		int seen = 0;
		for (int j = i + 1; j < nf; j++)
			if (strcmp(names[i], names[j]) == 0) seen = 1;
		if (!seen) tab_get(&flat, names[i])->total++;
	}
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
	fprintf(o, "#   self%%     self   total%%    total  symbol\n");
	for (size_t i = 0; i < n; i++) {
		double s = n_samples ? 100.0 * v[i].self / n_samples : 0;
		double t = n_samples ? 100.0 * v[i].total / n_samples : 0;
		fprintf(o, "  %6.2f %8" PRIu64 "  %6.2f %8" PRIu64 "  %s\n",
		        s, v[i].self, t, v[i].total, v[i].key);
	}
	free(v);
}

static void emit_folded(FILE *o) {
	size_t n;
	struct ent *v = sorted(&folded, &n);
	for (size_t i = 0; i < n; i++)
		fprintf(o, "%s %" PRIu64 "\n", v[i].key, v[i].self);
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
	        "usage: %s [-F HZ] [-o FILE] [--folded] -- ./prog [args...]\n"
	        "  -F HZ      sample frequency, default 997\n"
	        "  -o FILE    write the profile here (default: stderr)\n"
	        "  --folded   collapsed stacks (\"main;foo;bar 42\") instead of a flat profile\n"
	        "\n"
	        "  --no-cfi   unwind with frame pointers only, ignoring .eh_frame. For\n"
	        "             the test suite: it shows what the fallback alone recovers.\n"
	        "\n"
	        "  --cfi-dump FILE   read hex addresses on stdin, print the .eh_frame\n"
	        "                    unwind row for each in readelf's notation. A debug\n"
	        "                    mode, and the hook the test suite uses to diff this\n"
	        "                    CFI reader against readelf --debug-dump=frames-interp.\n",
	        prog);
	exit(2);
}

int main(int argc, char **argv) {
	int hz = 997, want_folded = 0;
	/* Checked before anything else because it profiles nothing: it reads one
	 * ELF file and prints unwind rows. */
	if (argc == 3 && !strcmp(argv[1], "--cfi-dump")) return cfi_dump(argv[2]);
	const char *out = NULL;
	int i = 1;

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		else if (!strcmp(argv[i], "--folded")) want_folded = 1;
		else if (!strcmp(argv[i], "--no-cfi")) no_cfi = 1;
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
		a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
		                PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
		a.sample_regs_user = REGMASK;
		a.sample_stack_user = STACK_BYTES;

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

	tab_init(&folded, 1 << 12);
	tab_init(&flat, 1 << 10);

	/* Release the child, then learn its address space. */
	if (write(pfd[1], "g", 1) != 1) die("write: %s", strerror(errno));
	close(pfd[1]);
	await_exec(pid, target[0]);

	struct pollfd *pfds = xmalloc(nrings * sizeof *pfds);
	for (int c = 0; c < nrings; c++) {
		pfds[c].fd = rings[c].fd;
		pfds[c].events = POLLIN;
	}

	int status = 0, reaped = 0;
	while (!reaped) {
		poll(pfds, nrings, 20);
		refresh_maps(pid); /* before draining, so these samples can be resolved */
		for (int c = 0; c < nrings; c++) drain(&rings[c]);
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid) reaped = 1;
		else if (w < 0 && errno != EINTR) break;
	}
	/* The child is gone but its last samples are still in the rings. */
	for (int c = 0; c < nrings; c++) drain(&rings[c]);
	free(pfds); /* LeakSanitizer reports it otherwise, and the suite runs
	             * a clean ASan build as an assertion, so a known leak would
	             * have to be either fixed or excused. This one was real. */

	FILE *o = stderr;
	if (out) {
		o = fopen(out, "w");
		if (!o) die("%s: %s", out, strerror(errno));
	}
	if (want_folded) emit_folded(o);
	else emit_flat(o);
	if (o != stderr) fclose(o);

	/* Always on stderr: never on stdout, because a profiler that pollutes the
	 * target's stdout is not transparent and cannot be dropped into a pipeline. */
	fprintf(stderr, "%s: %" PRIu64 " samples, %" PRIu64 " lost, %" PRIu64
	                " truncated stacks, %s @ %d Hz, %d/%d cpus\n",
	        prog, n_samples, n_lost, n_trunc, evname, hz, nrings, ncpu);
	fprintf(stderr, "%s: unwind: %" PRIu64 " frames via .eh_frame CFI, %" PRIu64
	                " via frame pointers (%" PRIu64 "/%" PRIu64 " samples fell back)\n",
	        prog, n_cfi_frames, n_fp_frames, n_fp_samples, n_samples);
	if (n_samples == 0)
		fprintf(stderr, "%s: no samples -- target too short-lived, or off-CPU the whole time\n", prog);

	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}
