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

printf '\n%d passed, %d failed, %d skipped\n' "$P" "$F" "$S"
[ "$F" -eq 0 ]
