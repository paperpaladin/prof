# PLAN

## Contract

`prof [-F HZ] [-o FILE] [--folded] -- ./prog args...` runs `./prog args...` to
completion, samples its user-space instruction pointer and stack at `HZ` per
second, and writes a profile of where it spent CPU time.

- Sampling mechanism: `perf_event_open(2)`, `PERF_TYPE_HARDWARE`/`CPU_CYCLES`
  if a PMU is reachable, else `PERF_TYPE_SOFTWARE`/`CPU_CLOCK`, else
  `SW_TASK_CLOCK`. One event per CPU, `pid=target`, `inherit=1`,
  `enable_on_exec=1`, `exclude_kernel=1`. The summary line names which event
  was used; `prof` never claims a mechanism it did not get.
- Stacks come from `PERF_SAMPLE_REGS_USER` + `PERF_SAMPLE_STACK_USER` and a
  frame-pointer (saved-RBP) walk done in `prof`, not by the kernel.
- Symbols come from `.symtab`, falling back to `.dynsym`, parsed from the ELF
  files named in `/proc/PID/maps`; unresolvable addresses print as
  `module+0xoffset`, never as a silent blank.
- Two output formats: a flat profile (self/total samples per symbol) by
  default, collapsed stacks (`main;foo;bar 42`) under `--folded`.
- Output goes to stderr, or to `-o FILE`. Never to stdout.
- `prof` exits with the target's exit status, or `128+signal` if it was
  signalled, or `2` on a usage/internal error, or `3` if no sampling event
  could be opened at all (with the relevant sysctl values quoted).
- Requires no root and changes no sysctl. C11 + gcc + Linux headers; no
  external libraries, no libelf.

## Hard invariant

Two of them, and every commit is checked against both:

1. **Profiling does not change the profiled program.** Its stdout is
   byte-identical and its exit status is passed through unchanged. Nothing is
   injected into its address space, no signal handler is installed in it, no
   environment variable is set for it, and its stdin/stdout/stderr are its own.
   Measured overhead at 997 Hz is inside run-to-run noise.
2. **A deliberately planted hot loop is always found, and named.** If `hot()`
   is built to burn 100 units against `warm()`'s 10 and `cold()`'s 1, then
   `hot` is the top symbol of the flat profile with >60% of samples and >3x
   `warm`. A profiler that cannot find a planted hot loop, or that finds it but
   calls it `0x55a3f21`, has failed regardless of what else works.

## Oracle

Ranked by how much a failure would matter. All of it is ground truth we
construct, none of it is a comparison against another profiler.

1. **Transparency** -- target stdout byte-identical, exit status `7` preserved,
   `-o FILE` keeps the profile out of stdout. A profiler you cannot trust not
   to perturb the program is useless even when its numbers are right.
2. **The planted ratios** -- `hot` >60% of samples (built as ~90%), `hot` >3x
   `warm` (built as 10x), `warm` > `cold`. The whole point.
3. **Symbol naming** -- the hottest symbol is literally `hot`. Catches a wrong
   load bias, a missed `.symtab`, an off-by-one in the address binary search:
   all failures that still produce beautiful output.
4. **Stack plausibility** -- `main;level1;level2;hot` appears verbatim in the
   folded output, and every folded line is exactly two fields so flamegraph
   tools accept it. Catches a unwinder that drops or invents a frame.
5. **The address space is known completely** -- symbols still resolve when the
   target execs a second image, and every stack reaches past `main` into the C
   runtime entry frame. Catches an incomplete `/proc/PID/maps` snapshot, whose
   failure mode is *non-deterministic* truncation and therefore worse than
   consistent truncation.
6. **The sampler actually samples periodically** -- 4x the frequency gives ~4x
   the samples, 2x the run time gives ~2x the samples. The only check that can
   catch a sampler firing once per run and extrapolating, which every other
   assertion above would happily pass.
7. **No silent loss** -- `PERF_RECORD_LOST` reports zero at 1997 Hz, proving the
   non-blocking drain loop keeps up with the kernel.

Harness prints `N passed, M failed, K skipped` and exits nonzero on any
failure. It skips (not passes) with a printed reason if `perf_event_open`
is unavailable or fewer than 50 samples were collected, because a green
"0 failed" on an un-run test is a lie.

## Non-goals

- Reimplementing `perf report`: no TUI, no call-graph tree view, no diffing, no
  `perf.data` format. Collapsed stacks hand off to existing flamegraph tools.
- System-wide or attach-to-running-PID profiling. Both need `paranoid <= 1` or
  `CAP_PERFMON`, and the contract is "needs no root".
- Kernel stacks. Forbidden at `paranoid >= 2` and unwindable only with kernel
  symbols we are not allowed to read.
- Off-CPU/wall-clock profiling (where a process is blocked, as opposed to where
  it burns CPU). Different mechanism entirely: scheduler tracepoints.
- JIT symbol maps (`/tmp/perf-PID.map`), C++ demangling, separate debuginfo
  files, compressed debug sections, 32-bit or big-endian ELF.
- A `setitimer`/`SIGPROF` in-process sampler. It was the stated fallback if
  `perf_event_open` had been unavailable; it is not, so building one would be
  dead code. It also violates invariant 1 by construction: it runs in the
  target's address space, steals `SIGPROF`, and cannot call `malloc` or
  `fprintf` from the handler without risking a deadlock when a sample lands
  inside the allocator.
- Windows, macOS, anything without `perf_event_open`.

## Commit sequence

Each of these builds, and each passes the tests that exist at it. Two pairs are
deliberately red-then-green, because the oracle caught something and the symptom
is worth having in history rather than described from memory.

- [x] `skeleton` -- fork/exec a target, pass its exit status back, park the child
      on a pipe between fork and exec.
      why: not perturbing the target outranks measuring it, and the perf events
      have to be attached in exactly that gap, so process control comes first.
- [x] `mechanism` -- probe `perf_event_paranoid`, ladder from HW cpu-cycles to
      SW_CPU_CLOCK to SW_TASK_CLOCK, report which one opened.
      why: the project is infeasible if an unprivileged user cannot sample, and
      the answer (paranoid=2, hardware cycles available) had to come from a
      running program. Naming the event that actually opened is what stops the
      output claiming a mechanism it did not get.
- [x] `ring buffer` -- per-CPU events, mmap'd buffers, non-blocking drain,
      `PERF_RECORD_LOST` accounting.
      why: `perf_mmap()` returns EINVAL for a per-task event with `inherit=1`, so
      following threads forces per-CPU events. And a drain that blocks on the
      child silently loses most of its samples while still printing a profile.
- [x] `samples` -- decode the `PERF_RECORD_SAMPLE` layout, aggregate into a
      string-keyed table, emit a flat profile of raw addresses.
      why: the record body is untagged fields in a fixed documented order, so the
      parser and its field order comment are the thing to get right once.
- [x] `maps` -- parse `/proc/PID/maps`, attribute addresses as `module+offset`.
      why: half of naming an address is knowing which file is mapped there, and
      `module+offset` survives ASLR where a raw address does not.
- [x] `elf` -- hand-rolled `.symtab`/`.dynsym` reader, per-`PT_LOAD` load bias,
      address-to-symbol binary search.
      why: a load bias computed as `start - pgoff` is wrong for most segments
      under `-z separate-code`, and the failure mode is naming the function a
      page away -- invisible in the output.
- [x] `unwind` -- `REGS_USER` + `STACK_USER` capture and a saved-RBP walk.
      why: a flat profile says which function is hot; only a stack says why.
- [x] `output` -- collapsed/folded stacks for flamegraph tools.
      why: the format every existing flamegraph renderer consumes, which is the
      whole reason not to write a renderer here.
- [x] `oracle` -- `t/workload.c` with 100:10:1 planted ratios and a fixed chain.
      why: up to this point "it works" meant the output looked plausible, which
      is a standard a wrong profiler meets easily.
- [x] `harness: transparency` -- stdout byte-identical, exit status preserved,
      `-o` keeps the profile out of stdout.
      why: ranked first because it is the one group that is still meaningful on a
      kernel where sampling is unavailable.
- [x] `harness: ratios and naming` -- hot >60% and >3x warm; top symbol is `hot`.
      why: the planted ratios are the point, and symbolisation is the failure
      that hides best.
- [x] `harness: the exact call chain` -- RED. `main;level1;level2;hot` asserted,
      and `level2` is missing.
      why: the first thing the oracle caught. Committed red so the symptom is in
      history and bisect can land on it.
- [x] `fix: leaf frames` -- GREEN. GCC 14 elides the prologue entirely for a leaf
      needing no stack slot, even under `-fno-omit-frame-pointer`; `BURN` keeps a
      `volatile` local to force it back.
      why: a frame never pushed cannot be walked, so this is a limitation of FP
      unwinding that only CFI removes, and it belongs in the README not in a
      workaround nobody can explain.
- [x] `harness: the maps snapshot is complete` -- RED. Nothing resolves at all
      when the target execs a second image; and intermittently, stacks stop at
      `main`.
      why: the second thing the oracle caught, and the deterministic exec form is
      what pinned down an intermittent libc-truncation symptom.
- [x] `fix: refresh maps every drain` -- GREEN. A single snapshot races `ld.so`
      and can never see a later exec or `dlopen`.
      why: re-reading `/proc/PID/maps` costs a 4 KB read; parsing an ELF symbol
      table still happens once per object.
- [x] `harness: scaling and loss` -- 4x Hz gives ~4x samples, 2x run time gives
      ~2x samples, zero lost at 1997 Hz.
      why: the only assertions that can catch a sampler which fires once and
      extrapolates -- every other check in the suite would pass one.
- [x] `README` -- real transcript, and a limitations list longer than the design
      section.
      why: a profiler's limitations decide whether its output can be trusted.
- [x] `PLAN` -- this file.
      why: so later work is judged against something written down.

### Milestone 1 -- make it work on real optimised binaries

- [ ] DWARF `.eh_frame` CFI unwinding: parse `.eh_frame_hdr`'s binary search
      table, decode CIE/FDE, run the CFA program for the sampled PC, recover
      the caller's PC/SP/BP from the captured stack.
      why: this is the one that matters. Every system binary and every
      `-O2` build omits frame pointers, so today `prof` produces depth-1 stacks
      for anything it did not compile itself -- the flat profile is right and
      the flamegraph is useless. Nothing else on this list is worth doing first.
- [ ] Register the unwinder choice per mapping: CFI where `.eh_frame` exists,
      frame pointers as the fallback, and report per-sample which was used.
      why: a profiler that silently degrades is how you end up believing a
      wrong flamegraph; the degradation should be visible in the output.
- [ ] Consume `PERF_RECORD_MMAP2`/`COMM`/`FORK`/`EXIT` from the ring buffer
      instead of polling `/proc/PID/maps`.
      why: removes the last race and the last reason we need the target to
      still be alive, and gets `dlopen`/`munmap` exactly right instead of
      approximately right.
- [ ] Per-PID address spaces so a target that `fork()`s is attributed per
      process rather than against one merged symbol table.
      why: `inherit=1` already samples children; their addresses are currently
      resolved against the parent's maps, which is wrong for any PIE child.
- [ ] Inline-frame expansion from `.debug_line`/`.debug_info`.
      why: at `-O2` the function you care about is usually inlined into its
      caller, so "where the time goes" and "which symbol the address is in"
      stop being the same question.
- [ ] C++ (Itanium ABI) demangling.
      why: `_ZN4Foo3barEi` in a flamegraph is a symbol lookup task for the
      reader, and the mangling grammar is self-contained.
- [ ] JIT symbol support: read `/tmp/perf-PID.map`.
      why: it is the established convention, it is a text file, and without it
      every JVM/V8/LuaJIT profile is a wall of `module+0x...`.
- [ ] Separate debuginfo: follow `.gnu_debuglink` and the build-id path under
      `/usr/lib/debug`.
      why: distro binaries are stripped, so the symbols that would name
      `libc.so.6+0x1ca7` as `__libc_start_call_main` are sitting on disk
      unread.
- [ ] `-p PID` to attach to a running process, degrading cleanly when
      `paranoid` forbids it.
      why: the interesting performance problems are in processes already
      running, and "restart it under the profiler" is often not an option.
- [ ] Wall-clock/off-CPU mode via `sched:sched_switch`, merged with the CPU
      profile.
      why: half of real latency problems are a process waiting, and a
      cycles-based profiler reports those as "no samples".
- [ ] Mid-run streaming output and a sample cap, so a long-running or
      high-frequency target cannot grow the aggregation table without bound.
      why: today the folded table lives in memory for the whole run; the
      ceiling has not been hit yet but it is a ceiling.
- [ ] Self-profiling CI check: assert the drain loop reports zero lost samples
      at the kernel's `perf_event_max_sample_rate` on a stack-deep target.
      why: the current "0 lost at 1997 Hz" assertion only proves the loop keeps
      up with small samples on a shallow stack.
