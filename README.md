# prof

A sampling CPU profiler in ~1600 lines of C11 that drives `perf_event_open(2)` itself, unwinds with DWARF `.eh_frame` CFI (frame pointers only as a fallback), reads symbols straight out of the ELF `.symtab`/`.dynsym`, and prints a flat profile or flamegraph-ready collapsed stacks.

```
prof [-F HZ] [-o FILE] [--folded] [--no-cfi] -- ./prog args...
```

No libelf, no libunwind, no libdw, no libdwfl. `gcc prof.c -o prof` is the whole build.

The CFI unwinder is what makes it usable on code it did not compile. Profiling
`python3` with frame pointers alone gives 131 stacks of maximum depth **1**;
with `.eh_frame` CFI the same workload gives 137 stacks of maximum depth **44**.
Both numbers come from the same binary one command apart, which is what
`--no-cfi` is for.

## Why this exists

`perf record` is a 200k-line program and `perf report` is another. The actual mechanism underneath is small enough to hold in your head. Ask the kernel for a sampling event, mmap a ring buffer, read `{RIP, RBP, RSP, 8KB of stack}` out of it a thousand times a second, walk the saved-RBP chain, and binary-search a sorted symbol table. This is that, with nothing in the way, so you can read it, break it, and see which part of a profiler is load-bearing.

The second reason is that profilers are unusually easy to get silently, confidently wrong. A sampler that fires once per run still prints a plausible-looking profile. An unwinder that drops one frame still prints plausible-looking stacks. A symboliser that is off by one load bias still prints plausible-looking names. None of those failures look like failures. So this repo has an oracle.

## The oracle

`t/workload.c` is built so its correct profile is known before `prof` ever runs:

| function | work | expected share |
|---|---|---|
| `hot()` | 100 units | ~90.1% |
| `warm()` | 10 units | ~9.0% |
| `cold()` | 1 unit | ~0.9% |

Three separate copies of the same loop, with a 10x and 100x iteration count. Not a shared `burn()` helper, if all three called one helper, every sample would land in the helper and the ratios would be untestable. And `hot()` is reachable only via `main -> level1 -> level2 -> hot`, so the correct stack is fixed by the source, not by measurement.

`t/run.sh` then asserts 22 things, in roughly descending order of how badly a failure would matter:

1. Transparency. The target's stdout is byte-identical and its exit status (a distinctive `7`) passes through unchanged, with or without the profiler. A profiler that perturbs its target is worse than no profiler.
2. The planted hot loop is found. `hot()` takes >60% of samples (built to be ~90%) and >3x the samples of `warm()` (built as 10x), and `warm()` outweighs `cold()`.
3. The hottest symbol is named `hot`. Symbolisation is where profilers fail quietly. A profile full of `0x7f...` addresses is a profile nobody reads.
4. The chain `main;level1;level2;hot` appears in the folded output, and every folded line is exactly `stack count` so flamegraph tooling can eat it.
5. The address space is known completely. Symbols must still resolve when the target execs a second image (`sh -c "exec t/workload 1"`, the shape of every wrapper script), and every stack must reach past `main` into the C runtime entry frame. Both catch an incomplete `/proc/PID/maps` snapshot, which truncates stacks *non-deterministically*, strictly worse than truncating them always, because the two profiles are indistinguishable.
6. The sampler really samples. 4x the frequency gives ~4x the samples; 2x the run time gives ~2x the samples. This is the test that catches a "profiler" that fires once and extrapolates: the single most embarrassing possible bug here, and invisible to every other check.

   Asserted at **3.0x and 1.75x**, not at 4x and 2x, because this is a sampling profiler and the ratio is a claim about this CPU under this load. Measured here across several runs: 3.9x-4.4x and 1.9x-2.1x. A sampling ratio, so it is a range and not a figure. The bars were **1.6x and 1.5x** until an audit pointed out that a sampler realising 40% of the requested frequency increase would pass, while the pass message prints the measured ratio, so the output read as though the stated claim were being enforced. Stating both numbers because the gap between what is claimed and what is asserted is the reader's business.
7. Nothing was dropped. `PERF_RECORD_LOST` must report zero at 1997 Hz, i.e. the drain loop keeps up.

### The oracles for the CFI unwinder

The workload cannot be the oracle for `.eh_frame` parsing: it says what the
call chain is, not what the unwind tables say. Two outside authorities do that,
and neither of them is this program.

**`readelf --debug-dump=frames-interp`** prints the interpreted CFI table for
every FDE in a file: one row per address at which the rules change, a column
for the canonical frame address and one per callee-saved register.
`prof --cfi-dump FILE` prints the same rows in the same notation for whatever
addresses it is handed on stdin, so `t/cfi_cmp.py` can diff them directly, with
no running target and no profile in the way. It probes the start of every row,
the byte after it, and the last byte it covers, and compares **every column
readelf printed** plus a `u` for every register readelf did *not* print a
column for.

```
$ python3 t/cfi_cmp.py ./prof /usr/bin/python3 /usr/bin/gdb       /lib/x86_64-linux-gnu/libstdc++.so.6 /lib/x86_64-linux-gnu/libc.so.6
cfi vs readelf: 818786 rows agree, 0 wrong, 0 refused;
30973/30973 uncovered addresses refused correctly; 42234/42234 FDEs compared
```

Wrong answers and refusals are counted separately, because they are different
failures: a refusal costs a stack, a wrong answer puts a plausible lie in a
flamegraph. The addresses in the gaps *between* FDEs are the other half of it.
An unchecked binary search hands those the preceding function's rules, and
nothing else in the comparison notices; deleting the FDE range check leaves all
818786 rows still agreeing and turns `30973/30973` into `0/30973`.

**`gdb`'s `backtrace`** is the oracle for the runtime side. `t/workload_nofp`
is built `-O2 -fomit-frame-pointer`, the way every distro binary is built:
there is no saved-RBP chain to walk at all. gdb reads the same `.eh_frame` and
prints the true chain, so the suite takes the expected chain *from gdb's
output* rather than hardcoding it, and requires `prof --folded` to contain it.
The same run then repeats with `--no-cfi`, where it must collapse to depth 1.
Without that second half the first proves nothing about CFI.

**`gas`**, for the opcodes nothing else emits. A sweep over everything above
(500k CFI opcodes) finds only eleven distinct ones, so six opcodes this parser
implements were unreachable from any real file and three mutations of them
survived with 73246 rows still agreeing. `t/cfi_probe.s` writes them by hand
with `.cfi_*` directives and `.cfi_escape`, gas encodes them, readelf says what
they mean, and prof has to agree. It is the same trick as `terminfo`'s
`tic`-compiled probe entry for stack operators no installed terminal uses.

Measured on this machine, one run: `hot` 90.3% of samples, `hot/warm` 10.2x, `warm` 63 vs `cold` 6 samples, frequency scaling 4.05x for 4x Hz, duration scaling 1.96x for 2x reps. All five move run to run. The assertions are the bars above, not these.

## `make test`

```
$ make test
ok    build: prof and workload are present
ok    transparency: exit status passed through unchanged (7)
ok    transparency: target stdout byte-identical
ok    -o FILE writes the profile there, not into the target's stdout
ok    symbolisation: hottest symbol is named 'hot'
ok    oracle: hot() holds 90.15% of samples (built to be ~90%, need >60%)
ok    oracle: hot/warm = 10.1x (built as 10x, need >3x)
ok    oracle: warm(60) > cold(6), as built (10:1)
ok    unwind: folded output contains main;level1;level2;hot
ok    unwind: folded output is well formed ('stack count' per line, 3 lines)
ok    maps: symbols still resolve when the target execs another image
ok    maps: every stack reaches past main into the entry frame [_start][_start][_start]
ok    scaling: 4x the frequency gave 4.19x the samples (499Hz=330, 1997Hz=1384)
ok    scaling: 2x the run time gave 2.02x the samples (1 rep=330, 2 reps=666)
ok    ring buffer: 0 samples lost at 1997 Hz
ok    cfi: cfi vs readelf: 73960 rows agree, 0 wrong, 0 refused; 3581/3581 uncovered addresses refused correctly; 3948/3948 FDEs compared
ok    cfi: reproduces gdb's main;level1;level2;hot on an fp-less binary (4142 frames via CFI, 0 via fp)
ok    cfi: --no-cfi on the same binary collapses to depth 1 (3 stacks, none deeper)
ok    unwinder choice: no .eh_frame for this object, 2666 frames from the RBP walk and the chain is still right
ok    asan: the CFI path is clean under AddressSanitizer
ok    asan: an over-read past the .eh_frame copy is reported
ok    asan: an over-read past the sample record is reported

22 passed, 0 failed, 0 skipped
```

The `cfi:` line compares prof against readelf over this repo's own binaries and
the libc they link against; the 818786-row run above adds python3, gdb and
libstdc++, which are not assumed to exist on another machine. `make test`
builds three ASan binaries of prof as part of the suite, two of them with a
deliberate one-byte over-read: see "A buffer larger than its data" below.

Also clean under `valgrind -q --error-exitcode=9`, CFI path included (it
reports 51 lost samples under valgrind, which is the drain loop being slowed
by a factor of 50, not a defect). Measured cost of being profiled at 997 Hz:
0.84s vs 0.81-0.85s unprofiled, i.e. inside run-to-run noise, and the realised
sample rate (840 samples / 0.85 s = 988 Hz) matches the 997 Hz asked for.

## Using it

```
$ ./prof -- ./t/workload 1
workload: ok reps=1
# flat profile: 672 samples
#   self%     self   total%    total  symbol
   90.48      608   90.48      608  hot
    8.63       58    8.63       58  warm
    0.89        6    0.89        6  cold
    0.00        0  100.00      672  __libc_start_main
    0.00        0  100.00      672  _start
    0.00        0   90.48      608  level1
    0.00        0   90.48      608  level2
    0.00        0  100.00      672  libc.so.6+0x1ca7
    0.00        0  100.00      672  main
prof: 672 samples, 0 lost, 0 truncated stacks, hw cpu-cycles @ 997 Hz, 8/8 cpus
prof: unwind: 4028 frames via .eh_frame CFI, 0 via frame pointers (0/672 samples fell back)

$ ./prof --folded -- ./t/workload_nofp 1      # built -O2 -fomit-frame-pointer
workload: ok reps=1
_start;__libc_start_main;libc.so.6+0x1ca7;main;level1;level2;hot 636
_start;__libc_start_main;libc.so.6+0x1ca7;main;warm 60
_start;__libc_start_main;libc.so.6+0x1ca7;main;cold 6
prof: 702 samples, 0 lost, 0 truncated stacks, hw cpu-cycles @ 997 Hz, 8/8 cpus
prof: unwind: 4080 frames via .eh_frame CFI, 0 via frame pointers (0/702 samples fell back)
```

The second line of the summary is the per-mapping unwinder choice, reported
rather than implied: CFI where the object has an indexable `.eh_frame`, the
RBP walk where it does not, counted per frame, plus how many samples needed
the fallback at all. A profiler that silently degrades is how you end up
believing a wrong flamegraph.

`self` is samples whose innermost frame is this symbol; `total` is samples with this symbol anywhere in the stack. The profile goes to stderr unless you pass `-o FILE`, and never to stdout, that is what makes `prof, ./prog | consumer` safe. `libc.so.6+0x1ca7` is `__libc_start_call_main`, which a stripped distro libc does not export; `module+offset` is the honest answer there.

Pipe the folded form straight into a flamegraph:

```
./prof --folded -o out.folded -- ./prog && flamegraph.pl out.folded > out.svg
```

## Which sampling mechanism this actually uses

`perf_event_open(2)` directly, for real, no fallback was needed on this kernel. Measured here:

- `/proc/sys/kernel/perf_event_paranoid` = 2
- kernel 7.2.8-200.fc44.x86_64, unprivileged uid 1000, no root, no sysctl changed
- `/proc/sys/kernel/perf_event_max_sample_rate` = 6000 here (so `-F` above that is rejected by the kernel; `prof` says so). The kernel auto-throttles this sysctl, so it moves on its own, it read 14000 when this was written and `prof` quotes the live value, not this one.

`paranoid=2` forbids kernel-mode measurement but permits a user to sample their own processes, all this needs. Every event is opened with `exclude_kernel=1` and `exclude_hv=1`. The event tried first is `PERF_TYPE_HARDWARE`/`PERF_COUNT_HW_CPU_CYCLES`, the one that worked here (`hw cpu-cycles` in the summary line). If no PMU is exposed: a VM or container without PMU pass-through, it falls back to `PERF_TYPE_SOFTWARE`/`PERF_COUNT_SW_CPU_CLOCK` and then `SW_TASK_CLOCK`, both hrtimer-driven. The summary line always names which one it got, so you never have to guess. Those `setitimer`/`SIGPROF`-via-`LD_PRELOAD` and `/proc/PID/stat`-polling fallbacks described as contingencies were not implemented, because they were not needed and writing unused code would be worse than not writing it.

Two kernel details that shaped the design, both discovered by hitting them:

- The event is opened **per CPU** with `inherit=1` and `pid=child`, not as a single per-task event. `perf_mmap()` refuses a per-task event with `inherit=1` (`EINVAL`) because all inherited children would share one ring buffer. Per-CPU events are how `perf record` follows threads. That was stated here as "verified: a two-thread target yields both `main;spin` and `th;spin`", there is no such target in this repo and `git log --all` shows there never was, so the claim is removed, not restated. t/workload.c is single-threaded.
- The executable mappings are re-read from `/proc/PID/maps` on every drain iteration, not snapshotted once. A single early snapshot races `ld.so`: you see the main binary but not yet libc, every return address into libc then looks unmapped, and stacks truncate non-deterministically. It also has to happen while the target is alive, since `/proc/PID/maps` disappears the instant it exits.

## Two assertions used to pass on no output at all

Both are the suite's defences against prof producing nothing, and both were
satisfied by prof producing nothing.

`maps: every stack reaches past main` compared the set of outermost frames
and required exactly one, not `main`. On an empty folded file the set is the
empty string, `printf '%s\n' "" | wc -l` is 1, and `"" != main` is true,
so all three conditions held, and the diagnostic helpfully printed `[]`.
This is the check the README ranks as guarding against the worst failure
mode here, non-deterministic stack truncation, and it was green on zero
stacks.

`unwind. Folded output is well formed` ran an `awk` program over the file.
awk over zero records runs no rules and exits 0.

Both now require the file to be non-empty first, and both report the line
count so the number is in the output, not implied.

The spelling of that guard matters too. `grep -c . file || echo 0` looks
right and is wrong: `grep -c` prints `0` and exits 1 when nothing
matches, so the `||` appends a second zero, the variable becomes `0\n0`, and
`[ -gt ]` becomes an integer-expression error. It fails in the right
direction by accident, and that is not the same as working. `wc -l < file`
does not have that problem.

## What does not work

- Frames removed by tail-call optimisation cannot be recovered, by anything that reads only `.eh_frame`. At `-O2` gcc turns `level2() { hot(); }` into `jmp hot`, so `level2` never gets a frame and is not on the stack to find. gdb prints it anyway, from `DW_TAG_call_site` data in `.debug_info`, which is a different section and a different project. `t/workload_nofp` is therefore built `-fno-optimize-sibling-calls`, because a test comparing prof against gdb has to compare real frames; the unmodified `-O2` build gives `main;hot` from prof and `main;level1;level2;hot` from gdb, and prof is right about the stack that exists.
- A CFA given by a DWARF expression is not evaluated. `DW_CFA_def_cfa_expression` appears 7 times in everything the suite reads (the PLT, and a little hand-written libc asm), and `DW_CFA_expression` register rules 17 times. Those frames fall back to the RBP walk, which in a PLT stub is usually the caller's RBP and therefore right by luck. A DWARF expression evaluator over the captured registers is the upgrade path. `prof --cfi-dump` prints `exp` for them, which is also what readelf prints, so the refusal itself is checked.
- Signal-trampoline frames are treated as ordinary ones. A CIE with `S` in its augmentation means the return address is the exact PC rather than the byte after a call, so one frame either side of a signal handler can attribute to the wrong line. Not asserted anywhere, because the suite has no signal-handling target.
- 32-bit DWARF only, and no `PE_indirect` encodings. A `0xffffffff` initial length (the 64-bit form) and an indirect pointer encoding, which needs the live GOT, are both refused rather than guessed at. No x86-64 toolchain emits either in `.eh_frame`.
- Objects with no `.eh_frame_hdr` search table get no CFI at all. The table is what makes the lookup a binary search over libc's 3900 FDEs instead of a scan per sample, and building one ourselves at load time is the upgrade path. In practice everything a linker produced has one; a bare `.o` does not.
- `.debug_frame` is not read. It is the same encoding with different pointer rules, and nothing that matters ships it without `.eh_frame`.
- Frame pointers remain the fallback, with all their old limitations, and the fallback is what runs for any object above. In particular GCC 14 on x86-64 omits the prologue entirely for a leaf that needs no stack slot, even under `-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`: sampling inside such a leaf yields the *caller's* RBP and one level of the chain vanishes. We measured exactly `main;level1;hot` with `level2` missing. `t/workload.c`'s `BURN` macro keeps one `volatile` local to force the frame back, and the comment there says so. CFI is what actually fixes it, which is why `t/workload_nofp` needs no such trick.
- No kernel stacks. `exclude_kernel=1` is mandatory at `paranoid >= 2` and we could not unwind kernel frames anyway. Time spent in syscalls is attributed to the user-space frame that called in.
- No JIT support. Symbols come from ELF files on disk. JVM/V8/LuaJIT code shows as `module+offset` or a bare address; `/tmp/perf-PID.map` is not read.
- No off-CPU profiling. A process blocked on I/O generates no cycles and no samples; `prof` says "no samples" instead of telling you where it was blocked.
- No inline-frame expansion. One address maps to one symbol. Inlined callees are attributed to the function they were inlined into (which is also why the workload builds with `-fno-inline`).
- 8KB of stack is copied per sample, so stacks deeper than that are truncated. The count of truncations is reported in the summary line, not hidden. (A figure of 92 truncations "from a two-thread test" used to sit here. That target does not exist in this repo and `n_trunc` is not asserted anywhere in the harness.)
- 64-bit little-endian ELF only. No 32-bit, no separate debuginfo files, no compressed debug sections, no C++ demangling.
- One snapshot of symbols per object. A `dlopen` is picked up (maps are refreshed continuously) but an unmap/remap at the same address is not.
- Symbols from every sampled process share one table, so a target that `fork()`s has its children's PIE addresses resolved against the parent's mappings. `inherit=1` samples them. Naming them correctly needs `PERF_RECORD_FORK`/`MMAP2`.

## Design

```
fork + pipe                 child blocks on read() until the events are armed
perf_event_open x ncpu      pid=child, cpu=N, inherit=1, enable_on_exec=1,
                            exclude_kernel=1, freq=HZ,
                            sample_type = IP|TID|REGS_USER|STACK_USER
mmap 1+64 pages each        one ring buffer per CPU
write(pipe) -> execvp       counting starts at the exec, not before
loop while child alive:     poll(20ms) -> refresh maps -> drain every ring
  drain:                    data_head/data_tail with acquire/release fences,
                            memcpy records out of the circular buffer
  per sample:               .eh_frame CFI walk, RBP chain where there is none
                            -> return addresses -> symbols
reap, drain once more       the last samples are still in the rings
emit                        flat profile or folded stacks, to stderr or -o FILE
exit                        with the child's status
```

Three things that are the way they are for a reason, each with the reason in the source:

- The drain loop never blocks on the child. `poll` with a 20 ms timeout plus `waitpid(WNOHANG)`, never a blocking `wait`. While nobody drains, the kernel keeps writing. Once `data_head` laps `data_tail` samples are dropped and reported as `PERF_RECORD_LOST`. The "0 lost at 1997 Hz" assertion in the test suite exists to keep this honest.
- Records are `memcpy`'d out of the ring, never read in place. A record straddles the end of the circular buffer whenever it feels like it; `ring_copy()` unwraps it.
- The load bias is computed per `PT_LOAD` segment, not by assuming `p_vaddr == p_offset`. Under `-z separate-code` a PIE has four `PT_LOAD`s, and guessing gets every symbol in a shared library wrong by a page or two, which looks like a working profiler that names the wrong function.

There is no signal handler and nothing runs in the target's address space, so the usual "do not allocate in the signal path" hazard does not apply to this design. It would apply immediately to the `setitimer`/`SIGPROF` fallback. A `SIGPROF` handler that called `malloc` or `fprintf` would deadlock the moment a sample landed inside the allocator, so such a sampler must write fixed-size records into a preallocated lock-free buffer and symbolise later. That is noted as a non-goal, not half-built.

Deliberate shortcuts are marked `shortcut:` in the source, each naming its ceiling and upgrade path.

## A buffer larger than its data turns the sanitizer off

Two buffers in the CFI work had that shape and both are now sized to the byte.

`.eh_frame_hdr` and `.eh_frame` are copied out of the file's `mmap` into
exactly-sized `malloc`s rather than parsed in place. A mapping is a whole
number of pages and the bytes between the end of the file and the end of the
last page read as zero, so a parser walking one byte past the end of
`.eh_frame` inside that mapping reads a plausible `0` and no sanitizer says
anything.

The ring-buffer record was a fixed 64 KB array holding a 4 KB sample, so an
over-read of a sample record landed 60 KB inside a live object. It is
`realloc`'d to exactly `h.size` now, which happens once per run because every
sample is the same size. Finding that shape immediately found a real over-read
next to it: the `dyn_size` field sits 8 bytes *after* the stack data, and the
bounds check covered the data but not the field.

`make test` keeps both honest with three ASan builds: a clean one, and two with
a deliberate one-byte over-read past each buffer that ASan must report.
Measured both ways. With either buffer rounded back up, both plants are
reported **zero** times; sized exactly, both are caught. That is the whole
argument for doing it.

## Mutation survivors

Everything above was checked by breaking the code and watching the suite go
red. 26 mutations of the CFI reader and unwinder were run; 22 were caught.
The four that were not, written down rather than left for someone to discover:

- **`DW_CFA_advance_loc4` reading 2 bytes instead of 4.** Genuinely
  equivalent on any input anyone can write: a 4-byte delta of 1 is
  `01 00 00 00`, the low two bytes still decode as 1, and the two leftover
  zero bytes decode as `DW_CFA_nop`. It would take a function spanning more
  than 64 KB between two unwind rows to tell the difference.
- **Hardcoding the search table at offset 12 in `.eh_frame_hdr`.** Equivalent
  for every encoding any linker emits (version byte, three encoding bytes, a
  4-byte `pcrel` `.eh_frame` pointer, a 4-byte count). Only an object using
  the 8-byte or `uleb128` forms would distinguish them, and none exists here.
- **Treating an undefined return-address rule as "no CFI" instead of "stop".**
  The frame-pointer fallback then runs at the outermost frame and also stops,
  so the output is identical. It is only a latent difference.
- **Looking up the return address itself rather than `ra - 1`.** The two land
  in the same FDE row except where a `call` is the last instruction of a
  function, which is what the `- 1` is for; no target in the suite has one. A
  workload ending in a call to a `noreturn` function would kill it.

## Files

```
prof.c          the profiler
Makefile        build, test, clean
t/workload.c    the oracle: known-by-construction CPU ratios and call chain
t/cfi_probe.s   hand-written CFI for the opcodes no real binary emits
t/cfi_cmp.py    diffs prof --cfi-dump against readelf, row by row
t/run.sh        the harness
PLAN.md         contract, invariant, oracle, non-goals, commit sequence
```

The Makefile builds the workload four ways on purpose, because each one is a
different unwinder. `t/workload` has frame pointers and CFI; `t/workload_nofp`
is `-O2 -fomit-frame-pointer`, so only CFI can walk it; `t/workload_nocfi` is
`-fno-asynchronous-unwind-tables`, so only frame pointers can; `t/cfi_probe` is
never run at all, only parsed.

## License

GPL-3.0. See `LICENSE`.
