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
if awk '{ if (NF != 2 || $2 !~ /^[0-9]+$/) exit 1 }' "$TMP/fold"; then
	ok "unwind: folded output is well formed ('stack count' per line)"
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
	outer_saw="$outer_saw[$(printf '%s' "$o" | tr '\n' ',')]"
	{ [ "$n" = 1 ] && [ "$o" != main ]; } || outer_ok=0
done
if [ "$outer_ok" = 1 ]; then
	ok "maps: every stack reaches past main into the entry frame $outer_saw"
else
	bad "maps: every stack must reach past main into the entry frame" \
	    "saw $outer_saw -- an incomplete /proc/PID/maps truncates the walk"
fi

printf '\n%d passed, %d failed, %d skipped\n' "$P" "$F" "$S"
[ "$F" -eq 0 ]
