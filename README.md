# prof

A sampling CPU profiler in ~750 lines of C11 that drives `perf_event_open(2)` itself, unwinds the frame-pointer chain by hand, reads symbols straight out of the ELF `.symtab`/`.dynsym`, and prints a flat profile or flamegraph-ready collapsed stacks.

```
prof [-F HZ] [-o FILE] [--folded] -- ./prog args...
```

No libelf, no libunwind, no libdw, no libdwfl. `gcc prof.c -o prof` is the whole build.

## Why this exists

`perf record` is a 200k-line program and `perf report` is another. The actual mechanism underneath is small enough to hold in your head: ask the kernel for a sampling event, mmap a ring buffer, read `{RIP, RBP, RSP, 8KB of stack}` out of it a thousand times a second, walk the saved-RBP chain, and binary-search a sorted symbol table. This is that, with nothing in the way -- so you can read it, break it, and see which part of a profiler is load-bearing.

The second reason is that profilers are unusually easy to get silently, confidently wrong. A sampler that fires once per run still prints a plausible-looking profile. An unwinder that drops one frame still prints plausible-looking stacks. A symboliser that is off by one load bias still prints plausible-looking names. None of those failures look like failures. So this repo has an oracle.

## The oracle

`t/workload.c` is built so its correct profile is known before `prof` ever runs:

| function | work | expected share |
|---|---|---|
| `hot()` | 100 units | ~90.1% |
| `warm()` | 10 units | ~9.0% |
| `cold()` | 1 unit | ~0.9% |

Three separate copies of the same loop, with a 10x and 100x iteration count. Not a shared `burn()` helper -- if all three called one helper, every sample would land in the helper and the ratios would be untestable. And `hot()` is reachable only via `main -> level1 -> level2 -> hot`, so the correct stack is fixed by the source, not by measurement.

`t/run.sh` then asserts 15 things, in roughly descending order of how badly a failure would matter:

1. **Transparency.** The target's stdout is byte-identical and its exit status (a distinctive `7`) passes through unchanged, with or without the profiler. A profiler that perturbs its target is worse than no profiler.
2. **The planted hot loop is found.** `hot()` takes >60% of samples (built to be ~90%) and >3x the samples of `warm()` (built as 10x), and `warm()` outweighs `cold()`.
3. **The hottest symbol is named `hot`.** Symbolisation is where profilers fail quietly; a profile full of `0x7f...` addresses is a profile nobody reads.
4. **The chain `main;level1;level2;hot` appears in the folded output**, and every folded line is exactly `stack count` so flamegraph tooling can eat it.
5. **The address space is known completely.** Symbols must still resolve when the target execs a second image (`sh -c "exec t/workload 1"`, the shape of every wrapper script), and every stack must reach past `main` into the C runtime entry frame. Both catch an incomplete `/proc/PID/maps` snapshot, which truncates stacks *non-deterministically* -- strictly worse than truncating them always, because the two profiles are indistinguishable.
6. **The sampler really samples.** 4x the frequency gives ~4x the samples; 2x the run time gives ~2x the samples. This is the test that catches a "profiler" that fires once and extrapolates -- the single most embarrassing possible bug here, and invisible to every other check.
7. **Nothing was dropped.** `PERF_RECORD_LOST` must report zero at 1997 Hz, i.e. the drain loop keeps up.

Measured on this machine: `hot` 90.3% of samples, `hot/warm` 10.2x, `warm` 72 vs `cold` 7 samples, frequency scaling 3.85x for 4x Hz, duration scaling 1.88x for 2x reps.

## `make test`

```
$ make test
cc -std=c11 -O2 -Wall -Wextra -g -o prof prof.c
cc -std=c11 -O1 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-inline -o t/workload t/workload.c
ok    build: prof and workload are present
ok    transparency: exit status passed through unchanged (7)
ok    transparency: target stdout byte-identical
ok    -o FILE writes the profile there, not into the target's stdout
ok    symbolisation: hottest symbol is named 'hot'
ok    oracle: hot() holds 90.27% of samples (built to be ~90%, need >60%)
ok    oracle: hot/warm = 10.2x (built as 10x, need >3x)
ok    oracle: warm(72) > cold(7), as built (10:1)
ok    unwind: folded output contains main;level1;level2;hot
ok    unwind: folded output is well formed ('stack count' per line)
ok    maps: symbols still resolve when the target execs another image
ok    maps: every stack reaches past main into the entry frame [libc.so.6+0x1ca7]
ok    scaling: 4x the frequency gave 3.85x the samples (499Hz=376, 1997Hz=1447)
ok    scaling: 2x the run time gave 1.88x the samples (1 rep=376, 2 reps=706)
ok    ring buffer: 0 samples lost at 1997 Hz

15 passed, 0 failed, 0 skipped
```

Also clean under `valgrind -q --error-exitcode=9`. Measured cost of being profiled at 997 Hz: 0.84s vs 0.81-0.85s unprofiled, i.e. inside run-to-run noise, and the realised sample rate (840 samples / 0.85 s = 988 Hz) matches the 997 Hz asked for.

## Using it

```
$ ./prof -- ./t/workload 1
workload: ok reps=1
# flat profile: 842 samples
#   self%     self   total%    total  symbol
   90.62      763   90.62      763  hot
    8.43       71    8.43       71  warm
    0.95        8    0.95        8  cold
    0.00        0   90.62      763  level1
    0.00        0   90.62      763  level2
    0.00        0  100.00      842  libc.so.6+0x1ca7
    0.00        0  100.00      842  main
prof: 842 samples, 0 lost, 0 truncated stacks, hw cpu-cycles @ 997 Hz, 8/8 cpus

$ ./prof --folded -- ./t/workload 1
workload: ok reps=1
libc.so.6+0x1ca7;main;level1;level2;hot 695
libc.so.6+0x1ca7;main;warm 67
libc.so.6+0x1ca7;main;cold 7
prof: 769 samples, 0 lost, 0 truncated stacks, hw cpu-cycles @ 997 Hz, 8/8 cpus
```

`self` is samples whose innermost frame is this symbol; `total` is samples with this symbol anywhere in the stack. The profile goes to **stderr** unless you pass `-o FILE`, and never to stdout -- that is what makes `prof -- ./prog | consumer` safe. `libc.so.6+0x1ca7` is `__libc_start_call_main`, which a stripped distro libc does not export; `module+offset` is the honest answer there.

Pipe the folded form straight into a flamegraph:

```
./prof --folded -o out.folded -- ./prog && flamegraph.pl out.folded > out.svg
```

## Status

### Which sampling mechanism this actually uses

`perf_event_open(2)` directly, for real -- **no fallback was needed on this kernel.** Measured here:

- `/proc/sys/kernel/perf_event_paranoid` = **2**
- kernel 7.2.8-200.fc44.x86_64, unprivileged uid 1000, no root, no sysctl changed
- `/proc/sys/kernel/perf_event_max_sample_rate` = 14000 (so `-F` above that is rejected by the kernel; `prof` says so)

`paranoid=2` forbids kernel-mode measurement but permits a user to sample their own processes, which is all this needs: every event is opened with `exclude_kernel=1` and `exclude_hv=1`. The event tried first is `PERF_TYPE_HARDWARE`/`PERF_COUNT_HW_CPU_CYCLES`, which is what worked here (`hw cpu-cycles` in the summary line). If no PMU is exposed -- a VM or container without PMU pass-through -- it falls back to `PERF_TYPE_SOFTWARE`/`PERF_COUNT_SW_CPU_CLOCK` and then `SW_TASK_CLOCK`, both hrtimer-driven. The summary line always names which one it got, so you never have to guess. The `setitimer`/`SIGPROF`-via-`LD_PRELOAD` and `/proc/PID/stat`-polling fallbacks described as contingencies were **not** implemented, because they were not needed and writing unused code would be worse than not writing it.

Two kernel details that shaped the design, both discovered by hitting them:

- The event is opened **per CPU** with `inherit=1` and `pid=child`, not as a single per-task event. `perf_mmap()` refuses a per-task event with `inherit=1` (`EINVAL`) because all inherited children would share one ring buffer. Per-CPU events are how `perf record` follows threads, and verified here: a two-thread target yields both `main;spin` and `th;spin`.
- The executable mappings are re-read from `/proc/PID/maps` on every drain iteration rather than snapshotted once. A single early snapshot races `ld.so`: you see the main binary but not yet libc, every return address into libc then looks unmapped, and stacks truncate **non-deterministically**. It also has to happen while the target is alive, since `/proc/PID/maps` disappears the instant it exits.

### What does not work

- **Frame pointers are required.** Stack unwinding walks the saved-RBP chain, so it only works on code built with `-fno-omit-frame-pointer`. On an optimised binary the stacks collapse to depth 1. Live demonstration -- the same workload at `-O2 -fomit-frame-pointer`:

  ```
  hot 671
  warm 69
  cold 6
  ```

  The flat profile survives (sample IPs are still correct, so `hot` is still 90%); the *stacks* are gone. Same with any system binary: `prof --folded -- python3 ...` gives 332 depth-1 frames. This is the honest ceiling of the approach and the reason **DWARF `.eh_frame` CFI unwinding is item 1 of Milestone 1** in `PLAN.md`, not something handwaved as done.
- **Even `-fno-omit-frame-pointer` is not enough for leaf functions.** GCC 14 on x86-64 omits the prologue entirely for a leaf that needs no stack slot, and `-mno-omit-leaf-frame-pointer` does not change that. Sampling inside such a leaf yields the *caller's* RBP, so one level of the chain vanishes -- we measured exactly `main;level1;hot`, with `level2` silently missing. `t/workload.c`'s `BURN` macro keeps one `volatile` local specifically to force the frame back; the comment there says so. Nothing in `prof` can fix this, only CFI can.
- **No kernel stacks.** `exclude_kernel=1` is mandatory at `paranoid >= 2` and we could not unwind kernel frames anyway. Time spent in syscalls is attributed to the user-space frame that called in.
- **No JIT support.** Symbols come from ELF files on disk. JVM/V8/LuaJIT code shows as `module+offset` or a bare address; `/tmp/perf-PID.map` is not read.
- **No off-CPU profiling.** A process blocked on I/O generates no cycles and no samples; `prof` says "no samples" rather than telling you where it was blocked.
- **No inline-frame expansion.** One address maps to one symbol. Inlined callees are attributed to the function they were inlined into (which is also why the workload builds with `-fno-inline`).
- 8KB of stack is copied per sample, so stacks deeper than that are truncated. The count of truncations is reported in the summary line rather than hidden -- a two-thread test shows 92 of them, from the thread's outermost frame.
- 64-bit little-endian ELF only. No 32-bit, no separate debuginfo files, no compressed debug sections, no C++ demangling.
- One snapshot of symbols per object; a `dlopen` is picked up (maps are refreshed continuously) but an unmap/remap at the same address is not.
- Symbols from every sampled process share one table, so a target that `fork()`s has its children's PIE addresses resolved against the parent's mappings. `inherit=1` samples them; naming them correctly needs `PERF_RECORD_FORK`/`MMAP2`.

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
  per sample:               RBP chain walk -> return addresses -> symbols
reap, drain once more       the last samples are still in the rings
emit                        flat profile or folded stacks, to stderr or -o FILE
exit                        with the child's status
```

Three things that are the way they are for a reason, each with the reason in the source:

- **The drain loop never blocks on the child.** `poll` with a 20 ms timeout plus `waitpid(WNOHANG)`, never a blocking `wait`. While nobody drains, the kernel keeps writing; once `data_head` laps `data_tail` samples are dropped and reported as `PERF_RECORD_LOST`. The "0 lost at 1997 Hz" assertion in the test suite exists to keep this honest.
- **Records are `memcpy`'d out of the ring, never read in place.** A record straddles the end of the circular buffer whenever it feels like it; `ring_copy()` unwraps it.
- **The load bias is computed per `PT_LOAD` segment**, not by assuming `p_vaddr == p_offset`. Under `-z separate-code` a PIE has four `PT_LOAD`s, and guessing gets every symbol in a shared library wrong by a page or two -- which looks like a working profiler that names the wrong function.

There is no signal handler and nothing runs in the target's address space, so the usual "do not allocate in the signal path" hazard does not apply to this design. It would apply immediately to the `setitimer`/`SIGPROF` fallback: a `SIGPROF` handler that called `malloc` or `fprintf` would deadlock the moment a sample landed inside the allocator, which is why such a sampler must write fixed-size records into a preallocated lock-free buffer and symbolise later. That is noted as a non-goal rather than half-built.

Deliberate shortcuts are marked `shortcut:` in the source, each naming its ceiling and upgrade path.

## Files

```
prof.c          the profiler
Makefile        build, test, clean
t/workload.c    the oracle: known-by-construction CPU ratios and call chain
t/run.sh        the harness
PLAN.md         contract, invariant, oracle, non-goals, commit sequence
```

## License

GPL-3.0. See `LICENSE`.
