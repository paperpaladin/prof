#!/bin/sh
# t/run.sh - the oracle harness.
#
# Every assertion here is checkable without trusting the profiler: the workload's
# CPU split is fixed by construction (100:10:1), its call chain is fixed by
# source, and its stdout and exit status are fixed by source. If prof disagrees
# with any of those, prof is wrong.
#
# Prints "N passed, M failed, K skipped" and exits nonzero on any failure.

cd "$(dirname "$0")/.." || exit 2
PROF=./prof
WL=t/workload
P=0; F=0; S=0
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

ok()   { P=$((P+1)); printf 'ok    %s\n' "$1"; }
bad()  { F=$((F+1)); printf 'FAIL  %s\n' "$1"; [ -n "$2" ] && printf '        %s\n' "$2"; }
skip() { S=$((S+1)); printf 'skip  %s\n' "$1"; [ -n "$2" ] && printf '        %s\n' "$2"; }

# true if awk evaluates the expression to nonzero
num() { awk "BEGIN{exit !($1)}"; }

samples() { sed -n 's/^prof: \([0-9]*\) samples.*/\1/p' "$1" | tail -1; }
selfcnt() { awk -v s="$2" '$NF==s{print $2; exit}' "$1"; }
selfpct() { awk -v s="$2" '$NF==s{print $1; exit}' "$1"; }

# --- 0. the pieces exist -------------------------------------------------
if [ -x $PROF ] && [ -x $WL ]; then
	ok "build: prof and workload are present"
else
	bad "build: prof and workload are present" "run make first"
	printf '\n%d passed, %d failed, %d skipped\n' "$P" "$F" "$S"
	exit 1
fi

# --- 1. transparency: stdout and exit status unchanged -------------------
# Done first and unconditionally: this one must hold even if sampling is
# unavailable, because a profiler that perturbs its target is useless either way.
$WL 1 >"$TMP/bare.out" 2>/dev/null; bare_rc=$?
$PROF -o "$TMP/p.flat" -- $WL 1 >"$TMP/prof.out" 2>"$TMP/p.err"; prof_rc=$?

if [ "$prof_rc" = 3 ]; then
	skip "perf_event_open unavailable" "$(tail -2 "$TMP/p.err")"
	printf '\n%d passed, %d failed, %d skipped\n' "$P" "$F" "$S"
	exit 1
fi

if [ "$bare_rc" = "$prof_rc" ]; then
	ok "transparency: exit status passed through unchanged ($bare_rc)"
else
	bad "transparency: exit status passed through unchanged" "bare=$bare_rc under prof=$prof_rc"
fi
if cmp -s "$TMP/bare.out" "$TMP/prof.out"; then
	ok "transparency: target stdout byte-identical"
else
	bad "transparency: target stdout byte-identical" "$(diff "$TMP/bare.out" "$TMP/prof.out" | head -4)"
fi
# -o must have captured the profile, leaving stdout to the target alone.
if [ -s "$TMP/p.flat" ] && ! grep -q 'flat profile' "$TMP/prof.out"; then
	ok "-o FILE writes the profile there, not into the target's stdout"
else
	bad "-o FILE writes the profile there, not into the target's stdout"
fi

n=$(samples "$TMP/p.err")
[ -n "$n" ] || n=0
if [ "$n" -lt 50 ]; then
	skip "too few samples ($n) to test attribution" "sampler fired but barely; nothing below is meaningful"
	printf '\n%d passed, %d failed, %d skipped\n' "$P" "$F" "$S"
	exit 1
fi

# --- 2. the hottest symbol is NAMED correctly ---------------------------
$PROF -o "$TMP/flat" -- $WL 1 >/dev/null 2>"$TMP/err"
top=$(grep -v '^#' "$TMP/flat" | head -1 | awk '{print $NF}')
if [ "$top" = hot ]; then
	ok "symbolisation: hottest symbol is named 'hot'"
else
	bad "symbolisation: hottest symbol is named 'hot'" "got '$top'"
fi

# --- 3. the planted ratios -------------------------------------------- --
hp=$(selfpct "$TMP/flat" hot)
hc=$(selfcnt "$TMP/flat" hot)
wc_=$(selfcnt "$TMP/flat" warm)
cc=$(selfcnt "$TMP/flat" cold)
: "${hp:=0}" "${hc:=0}" "${wc_:=0}" "${cc:=0}"

if num "$hp > 60"; then
	ok "oracle: hot() holds $hp% of samples (built to be ~90%, need >60%)"
else
	bad "oracle: hot() should hold >60% of samples" "got $hp%"
fi
if num "$wc_ > 0 && $hc > 3 * $wc_"; then
	ok "oracle: hot/warm = $(awk "BEGIN{printf \"%.1f\", $hc/$wc_}")x (built as 10x, need >3x)"
else
	bad "oracle: hot should take >3x the samples of warm" "hot=$hc warm=$wc_"
fi
if [ "$wc_" -gt "$cc" ]; then
	ok "oracle: warm($wc_) > cold($cc), as built (10:1)"
else
	bad "oracle: warm should outweigh cold" "warm=$wc_ cold=$cc"
fi

# --- 4. the call chain ---------------------------------------------------
$PROF --folded -o "$TMP/fold" -- $WL 1 >/dev/null 2>&1
if grep -q 'main;level1;level2;hot ' "$TMP/fold"; then
	ok "unwind: folded output contains main;level1;level2;hot"
else
	bad "unwind: folded output contains main;level1;level2;hot" "$(head -3 "$TMP/fold")"
fi
# A folded line is "stack count" and nothing else, or flamegraph tools choke.
#
# The non-emptiness test is not decoration: awk over zero records runs no
# rules and exits 0, so this passed on an empty file -- a loop that asserts
# nothing when there is nothing to assert about.
if [ "$(wc -l < "$TMP/fold")" -gt 0 ] &&
   awk '{ if (NF != 2 || $2 !~ /^[0-9]+$/) exit 1 }' "$TMP/fold"; then
	ok "unwind: folded output is well formed ('stack count' per line, $(wc -l < "$TMP/fold") lines)"
else
	bad "unwind: folded output is well formed" "$(head -3 "$TMP/fold")"
fi

# --- 5. the maps snapshot is complete ------------------------------------
# A target that execs a second image is the deterministic form of a bug that any
# single snapshot of /proc/PID/maps has: the snapshot is taken when the first
# image appears and never sees the second, so every address resolves to a bare
# hex number. Following an exec is also worth having on its own -- a wrapper
# script that ends in `exec ./real-binary` is the common way to launch things.
$PROF -F 300 --folded -o "$TMP/xexec" -- /bin/sh -c "exec $WL 1" >/dev/null 2>&1
if grep -q 'main;level1;level2;hot [0-9]' "$TMP/xexec"; then
	ok "maps: symbols still resolve when the target execs another image"
else
	bad "maps: symbols must still resolve when the target execs another image" \
	    "$(head -2 "$TMP/xexec")"
fi

# The same bug has an intermittent form on an ordinary target.
# The walk must reach past main into the process entry frame in libc. If the
# snapshot of /proc/PID/maps is incomplete, a return address into libc looks
# unmapped and the walk stops at main -- intermittently rather than always,
# which is strictly worse. Three runs, because intermittent is how it showed up.
outer_ok=1; outer_saw=
for r in 1 2 3; do
	$PROF --folded -o "$TMP/f$r" -- $WL 1 >/dev/null 2>&1
	o=$(awk -F';' '{print $1}' "$TMP/f$r" | sort -u)
	n=$(printf '%s\n' "$o" | wc -l)
	# wc -l, not `grep -c . || echo 0`: grep -c prints 0 AND exits 1 when
	# nothing matches, so the `||` appends a second 0 and the variable
	# becomes "0\n0" -- which makes `[ -gt ]` an integer-expression error
	# rather than a comparison. It fails in the right direction by
	# accident, which is not the same as working.
	nlines=$(wc -l < "$TMP/f$r")
	outer_saw="$outer_saw[$(printf '%s' "$o" | tr '\n' ',')]"
	# nlines first, and it is the whole point. On an EMPTY file o is the
	# empty string, `printf '%s\n' "" | wc -l` is 1, and "" != main is
	# true -- so the three conditions below were all satisfied by prof
	# having produced no output at all, and the diagnostic printed "[]".
	# This is the suite's defence against non-deterministic stack
	# truncation, which the README ranks as strictly worse than truncating
	# always, and it was green on zero stacks.
	# Every distinct outermost frame must be something other than main.
	#
	# This used to also require exactly one distinct outer frame, and that
	# was wrong. libc's entry frame returns from more than one address, so
	# a correct run can show both libc.so.6+0x1ca7 and libc.so.6+0x1caa;
	# neither is main, so the walk reached past main in both, and the test
	# failed anyway. It fired during an extract and recorded prof as
	# FAILING in the published figures while nothing was broken. A test
	# that fails when the code is right costs what a test that passes when
	# the code is wrong costs: you stop believing it.
	#
	# n is still computed, for the diagnostic.
	frames_ok=1
	[ "$nlines" -gt 0 ] || frames_ok=0
	while IFS= read -r frame; do
		[ -n "$frame" ] || continue
		case $frame in
		main|main\ *) frames_ok=0 ;;
		esac
	done <<EOF
$o
EOF
	[ "$frames_ok" = 1 ] || outer_ok=0
done
if [ "$outer_ok" = 1 ]; then
	ok "maps: every stack reaches past main into the entry frame $outer_saw"
else
	bad "maps: every stack must reach past main into the entry frame" \
	    "saw $outer_saw -- an incomplete /proc/PID/maps truncates the walk"
fi

# --- 6. the sampler actually samples periodically ------------------------
# Catches the classic broken profiler that fires once (or once per run) and then
# extrapolates: neither frequency nor duration would move the sample count.
$PROF -F 499  -o "$TMP/lo"   -- $WL 1 >/dev/null 2>"$TMP/lo.err"
$PROF -F 1997 -o "$TMP/hi"   -- $WL 1 >/dev/null 2>"$TMP/hi.err"
$PROF -F 499  -o "$TMP/long" -- $WL 2 >/dev/null 2>"$TMP/long.err"
lo=$(samples "$TMP/lo.err"); hi=$(samples "$TMP/hi.err"); lg=$(samples "$TMP/long.err")
: "${lo:=0}" "${hi:=0}" "${lg:=0}"

# 3.0 and 1.75, not 1.6 and 1.5.
#
# The claim in README and PLAN is "4x the frequency gives ~4x the samples,
# 2x the run time gives ~2x the samples". The bars were 1.6 and 1.5, so a
# sampler realising 40% of the requested frequency increase passed -- and
# the pass message prints the measured ratio, so the output read as though
# the stated claim were being enforced. Measured here: 4.11x and 2.00x.
#
# Still generous, deliberately. This is a sampling profiler and the ratio
# is a claim about this CPU under this load; the README says the suite
# fails under CPU competition. 3.0 against an observed 4.11 leaves room
# for that without leaving room for a sampler that barely scales at all.
if [ "$lo" -gt 0 ] && num "$hi > 3.0 * $lo"; then
	ok "scaling: 4x the frequency gave $(awk "BEGIN{printf \"%.2f\", $hi/$lo}")x the samples (499Hz=$lo, 1997Hz=$hi)"
else
	bad "scaling: raising the frequency must raise the sample count" "499Hz=$lo 1997Hz=$hi"
fi
if [ "$lo" -gt 0 ] && num "$lg > 1.75 * $lo"; then
	ok "scaling: 2x the run time gave $(awk "BEGIN{printf \"%.2f\", $lg/$lo}")x the samples (1 rep=$lo, 2 reps=$lg)"
else
	bad "scaling: doubling the run time must roughly double the sample count" "1rep=$lo 2reps=$lg"
fi

# --- 7. no silent data loss ---------------------------------------------
# `: "${lost:=0}"` turned "the sed found nothing" into "zero samples lost",
# which is the answer this assertion wants -- so the check passed whenever it
# could not read prof's report. Proved by injecting n_lost = 7 *and* renaming
# the word "lost" to "dropped" in prof.c's summary: the line printed
# "ok ring buffer: 0 samples lost at 1997 Hz" with seven samples genuinely
# lost. (Injecting n_lost alone is correctly caught; it is the pair that
# hides.) The two sibling defaults in this file fail in the safe direction;
# this one did not.
lost=$(sed -n 's/.*samples, \([0-9]*\) lost.*/\1/p' "$TMP/hi.err" | tail -1)
if [ -z "$lost" ]; then
	bad "ring buffer: 0 samples lost at 1997 Hz" \
	    "could not find a 'N samples, M lost' line in prof's output" \
	    "$(tail -3 "$TMP/hi.err")"
elif [ "$lost" -eq 0 ]; then
	ok "ring buffer: 0 samples lost at 1997 Hz"
else
	bad "ring buffer: samples were dropped at 1997 Hz" "$lost lost -- drain loop is too slow"
fi

# --- 8. DWARF .eh_frame CFI, against readelf --------------------------------
# readelf --debug-dump=frames-interp prints the interpreted CFI table for every
# FDE in a file: one row per address at which the unwind rules change, with a
# column for the CFA and one per callee-saved register. prof --cfi-dump prints
# the same rows in the same notation for whatever addresses it is given, so the
# two can be diffed directly, with no running target and no profile in the way.
#
# The count is printed and asserted, not just the exit status. A comparison of
# zero rows passes every other assertion in this group.
LIBC=$(ldd $PROF 2>/dev/null | awk '/libc\.so/{print $3}')
if ! command -v readelf >/dev/null 2>&1; then
	skip "cfi: unwind rows match readelf" "readelf is the only oracle for this group"
elif ! command -v python3 >/dev/null 2>&1; then
	skip "cfi: unwind rows match readelf" "python3 needed to drive the comparison"
else
	# prof and both workloads are gcc -O2/-O1 output; libc is a stripped,
	# hand-written-asm-containing, 3900-FDE optimised library, which is where
	# the rules this parser does not expect actually live.
	if python3 t/cfi_cmp.py $PROF $PROF $WL t/workload_nofp t/cfi_probe $LIBC >"$TMP/cfi" 2>&1; then
		rows=$(sed -n 's/^cfi vs readelf: \([0-9]*\) rows agree.*/\1/p' "$TMP/cfi")
		[ -n "$rows" ] || rows=0
		if [ "$rows" -ge 1000 ]; then
			ok "cfi: $(cat "$TMP/cfi")"
		else
			bad "cfi: too few rows compared to mean anything" "$(cat "$TMP/cfi")"
		fi
	else
		bad "cfi: unwind rows must match readelf" "$(tail -6 "$TMP/cfi")"
	fi
fi

# --- 9. the chain gdb finds in a binary with no frame pointers -------------
# t/workload_nofp is built -O2 -fomit-frame-pointer, i.e. the way every distro
# binary is built: there is no saved-RBP chain to walk at all. gdb reads the
# same .eh_frame and prints the true chain, so gdb is the oracle and the
# expected chain is taken FROM IT rather than written here.
NFP=t/workload_nofp
if ! command -v gdb >/dev/null 2>&1; then
	skip "cfi: reproduces gdb's backtrace on an fp-less binary" "gdb not installed"
elif [ ! -x $NFP ]; then
	bad "cfi: reproduces gdb's backtrace on an fp-less binary" "$NFP not built"
else
	# gdb prints innermost first; a folded stack is outermost first.
	chain=$(gdb -batch -ex 'break hot' -ex 'run 1' -ex bt $NFP 2>/dev/null |
	        awk '/^#[0-9]/ { print ($2 ~ /^0x/) ? $4 : $2 }' |
	        awk '{ f[++k]=$1 } END { s=""; for (i=k; i>=1; i--) s = s (s?";":"") f[i]; print s }')
	depth=$(printf '%s' "$chain" | awk -F';' '{print NF}')
	[ -n "$depth" ] || depth=0
	$PROF --folded -o "$TMP/nofp.fold" -- $NFP 1 >/dev/null 2>"$TMP/nofp.err"
	cfif=$(sed -n 's/^prof: unwind: \([0-9]*\) frames via .eh_frame CFI, \([0-9]*\) via.*/\1/p' "$TMP/nofp.err")
	fpf=$(sed -n 's/^prof: unwind: \([0-9]*\) frames via .eh_frame CFI, \([0-9]*\) via.*/\2/p' "$TMP/nofp.err")
	if [ "$depth" -lt 4 ]; then
		# An empty chain would make the grep below match every line, so this
		# guard is the whole test: "" is a substring of anything.
		bad "cfi: reproduces gdb's backtrace on an fp-less binary" \
		    "gdb gave a $depth-frame chain ('$chain'); expected at least main;level1;level2;hot"
	elif [ -z "$cfif" ] || [ -z "$fpf" ]; then
		bad "cfi: reproduces gdb's backtrace on an fp-less binary" \
		    "could not read the per-method frame counts from prof's summary"
	elif ! grep -Fq "$chain " "$TMP/nofp.fold"; then
		bad "cfi: reproduces gdb's backtrace on an fp-less binary" \
		    "gdb says $chain; prof folded output: $(head -1 "$TMP/nofp.fold")"
	elif [ "$cfif" -le 0 ]; then
		# Assert the new path RAN. Without this the case passes if some other
		# unwinder happens to find the chain.
		bad "cfi: the chain must come from CFI" "0 frames came via .eh_frame"
	elif [ "$fpf" -ne 0 ]; then
		bad "cfi: an fp-less binary must need no fp fallback" "$fpf frames came from the RBP walk"
	else
		ok "cfi: reproduces gdb's $chain on an fp-less binary ($cfif frames via CFI, $fpf via fp)"
	fi

	# ... and the same run with CFI switched off, which is what makes the case
	# above a claim about CFI rather than about the chain being easy to find.
	$PROF --no-cfi --folded -o "$TMP/nofp.nocfi" -- $NFP 1 >/dev/null 2>&1
	deep=$(awk -F';' 'NF>1' "$TMP/nofp.nocfi" | wc -l)
	lines=$(wc -l < "$TMP/nofp.nocfi")
	if [ "$lines" -gt 0 ] && [ "$deep" -eq 0 ]; then
		ok "cfi: --no-cfi on the same binary collapses to depth 1 ($lines stacks, none deeper)"
	else
		bad "cfi: --no-cfi on the same binary must collapse to depth 1" \
		    "$lines stacks, $deep of them deeper than one frame -- so the case above proves nothing"
	fi
fi

# --- 10. the per-mapping unwinder choice, and the fallback -----------------
# t/workload_nocfi has frame pointers and NO unwind tables, so prof must notice
# per address that there is no CFI, fall back to the RBP walk, still produce the
# right chain, and say in the summary which unwinder it used.
NOC=t/workload_nocfi
if [ ! -x $NOC ]; then
	bad "unwinder choice: falls back to frame pointers without .eh_frame" "$NOC not built"
else
	$PROF --folded -o "$TMP/noc.fold" -- $NOC 1 >/dev/null 2>"$TMP/noc.err"
	cfif=$(sed -n 's/^prof: unwind: \([0-9]*\) frames via .eh_frame CFI, \([0-9]*\) via.*/\1/p' "$TMP/noc.err")
	fpf=$(sed -n 's/^prof: unwind: \([0-9]*\) frames via .eh_frame CFI, \([0-9]*\) via.*/\2/p' "$TMP/noc.err")
	if [ -z "$cfif" ] || [ -z "$fpf" ]; then
		bad "unwinder choice: falls back to frame pointers without .eh_frame" \
		    "no per-method frame counts in prof's summary"
	elif ! grep -Fq 'main;level1;level2;hot ' "$TMP/noc.fold"; then
		bad "unwinder choice: the fallback must still find the chain" \
		    "$(head -1 "$TMP/noc.fold")"
	elif [ "$fpf" -le 0 ]; then
		bad "unwinder choice: the fallback must have been used" "$fpf frames from the RBP walk"
	else
		ok "unwinder choice: no .eh_frame for this object, $fpf frames from the RBP walk and the chain is still right"
	fi
fi

# --- 11. the sanitizer can see past the ends of the CFI buffers ------------
# A buffer larger than its data turns ASan off for everything inside it. The
# .eh_frame copy and the ring-buffer record are therefore sized to the byte,
# and these three cases are what keeps that true: a clean run, plus two builds
# with a deliberate one-byte over-read that ASan MUST report. Verified by
# rounding either buffer back up, at which point both plants go unreported.
for plant in 0 1 2; do
	case $plant in
	0) bin=./prof-asan;        want=clean; label="asan: the CFI path is clean under AddressSanitizer" ;;
	1) bin=./prof-asan-plant1; want=report; label="asan: an over-read past the .eh_frame copy is reported" ;;
	2) bin=./prof-asan-plant2; want=report; label="asan: an over-read past the sample record is reported" ;;
	esac
	if [ ! -x "$bin" ]; then
		bad "$label" "$bin not built (make test builds it)"
		continue
	fi
	$bin --folded -o "$TMP/asan.fold" -- $NFP 1 >/dev/null 2>"$TMP/asan.err"
	n=$(grep -c 'AddressSanitizer' "$TMP/asan.err")
	if [ "$want" = clean ] && [ "$n" -eq 0 ]; then
		ok "$label"
	elif [ "$want" = report ] && grep -q 'heap-buffer-overflow' "$TMP/asan.err"; then
		ok "$label"
	elif [ "$want" = clean ]; then
		bad "$label" "$(grep -m1 'AddressSanitizer' "$TMP/asan.err")"
	else
		bad "$label" "the planted over-read was NOT reported: the buffer is bigger than its data"
	fi
done

# --- 12. separate debuginfo ------------------------------------------------
# A distro libc ships .dynsym and no .symtab, so every internal frame used to
# print as libc.so.6+0xNNNN -- including the process entry frame, which is how
# it appeared in this repo's own README. The names are in a separate file named
# by the build-id, and readelf is the oracle twice over: it says the name is
# NOT in the library, and it says the name IS in the debug file, whose path is
# derived from the build-id readelf itself prints. prof only supplies the
# candidate names; it does not get to say whether they were obtainable.
$PROF --folded -o "$TMP/dbg.fold" -- $WL 1 >/dev/null 2>&1
bid=$(readelf -n "$LIBC" 2>/dev/null | sed -n 's/.*Build ID: \([0-9a-f][0-9a-f]*\).*/\1/p' | head -1)
dbg=
if [ -n "$bid" ]; then
	dbg="/usr/lib/debug/.build-id/$(printf '%s' "$bid" | cut -c1-2)/$(printf '%s' "$bid" | cut -c3-).debug"
fi
if ! command -v readelf >/dev/null 2>&1; then
	skip "debuginfo: names come from the separate debug file" "readelf is the oracle here"
elif [ -z "$LIBC" ] || [ -z "$bid" ]; then
	bad "debuginfo: names come from the separate debug file" \
	    "could not find the libc prof links against, or its build-id"
elif [ ! -r "$dbg" ]; then
	skip "debuginfo: names come from the separate debug file" \
	     "no debug file at $dbg (install the libc debug symbols)"
else
	# The NAME field of readelf's FUNC lines, exactly. The first version of
	# this used grep -Fw against the whole dump and reported success on
	# 'cold', one of the workload's own symbols, because '.' is a word
	# boundary to grep and libc has a function called __assert_fail_base.cold.
	# A membership test has to be against the set, not against the text.
	readelf -sW "$LIBC" 2>/dev/null | awk '$4=="FUNC"{print $NF}' | sort -u >"$TMP/libsyms"
	readelf -sW "$dbg" 2>/dev/null | awk '$4=="FUNC"{print $NF}' | sort -u >"$TMP/dbgsyms"
	found=
	# Every frame name prof produced, innermost field included.
	for n in $(tr ';' '\n' < "$TMP/dbg.fold" | sed 's/ [0-9]*$//' | sort -u); do
		grep -Fxq -- "$n" "$TMP/dbgsyms" || continue
		grep -Fxq -- "$n" "$TMP/libsyms" && continue
		found=$n
		break
	done
	if [ ! -s "$TMP/dbgsyms" ] || [ ! -s "$TMP/libsyms" ]; then
		bad "debuginfo: names come from the separate debug file" \
		    "readelf printed no symbols; nothing was actually compared"
	elif [ -n "$found" ]; then
		ok "debuginfo: '$found' is in $(basename "$dbg") and in no table of libc itself"
	else
		bad "debuginfo: names come from the separate debug file" \
		    "no frame name was obtainable only from the debug file: $(head -1 "$TMP/dbg.fold")"
	fi
	# ... and the symptom itself: no libc frame left as module+offset.
	if [ "$(wc -l < "$TMP/dbg.fold")" -gt 0 ] && ! grep -q 'libc[^;]*+0x' "$TMP/dbg.fold"; then
		ok "debuginfo: no libc frame is left as module+offset"
	else
		bad "debuginfo: no libc frame should be left as module+offset" \
		    "$(head -1 "$TMP/dbg.fold")"
	fi
fi

printf '\n%d passed, %d failed, %d skipped\n' "$P" "$F" "$S"
[ "$F" -eq 0 ]
