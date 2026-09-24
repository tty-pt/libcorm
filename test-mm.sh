#!/bin/sh -e
# test-mm.sh — 2B-5 mm-dialect gate (mm-plan/2B-5-IMPLEMENTATION.md).
#
# The documented pi-mm recipes (store → composed search → forget → reset)
# against REAL joint+stoma files. Payload shape is <DATE>:<TEXT> (explicit
# refs — auto-ref ingest cannot carry colon payloads, F1); the time leaf is
# always a bounded window (joint="a=A b=B", F2); -d needs the roster-backed
# file (F3); stoma stays derived (rebuilt from the primary at each open,
# D13); reset is the documented enumerate+forget loop (F5).
#
# Requires the real joint+stoma .so's; resolved from $CORM_AXIS_REAL_LIBS
# (colon list, default = the in-site submodule builds under
# external/lib{joint,stoma}/lib, relative to this script).
# Fresh local builds only (D4) — nothing installed.

td=$(mktemp -d)
trap 'rm -rf "$td"' EXIT

export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
corm=./bin/corm
fail=0

assert_eq() {
	name=$1
	expected=$2
	actual=$3
	if [ "$expected" = "$actual" ]; then
		echo "ok - $name"
	else
		echo "FAIL - $name"
		echo "  expected: $(printf '%s' "$expected" | tr '\n' '|')"
		echo "  actual:   $(printf '%s' "$actual" | tr '\n' '|')"
		fail=1
	fi
}

assert_ok() {
	name=$1
	expected_exit=$2
	actual=$3
	if [ "$expected_exit" = "$actual" ]; then
		echo "ok - $name"
	else
		echo "FAIL - $name (expected exit=$expected_exit actual=$actual)"
		fail=1
	fi
}

# --- resolve the real axis lib dirs (fresh local builds, D4) ---
# Same pattern as test-real.sh: in-site submodule builds relative to this
# script (explicit $CORM_AXIS_REAL_LIBS still overrides).
script_dir=$(dirname "$0")
real_libs=${CORM_AXIS_REAL_LIBS:-$script_dir/../libjoint/lib:$script_dir/../libstoma/lib}
axis_path=""
oIFS=$IFS
IFS=:
for d in $real_libs; do
	[ -n "$d" ] || continue
	axis_path="$axis_path${axis_path:+:}$d"
done
IFS=$oIFS
export CORM_AXIS_PATH=$axis_path
export LD_LIBRARY_PATH=$axis_path:$LD_LIBRARY_PATH

echo "=== preflight: real joint+stoma .so ==="
missing=0
for a in joint stoma; do
	found=0
	oIFS2=$IFS
	IFS=:
	for d in $axis_path; do
		if [ -r "$d/lib$a.so" ]; then
			found=1
			break
		fi
	done
	IFS=$oIFS2
	if [ "$found" = "1" ]; then
		echo "ok - lib$a.so present"
	else
		echo "FAIL - lib$a.so: not found in CORM_AXIS_PATH ($axis_path)"
		missing=1
	fi
done
if [ "$missing" = "1" ]; then
	echo "test-mm.sh: REAL AXIS LIBRARIES MISSING (1 FAIL)"
	exit 1
fi

echo "=== control: joint-only roster seed + forget persists ==="
"$corm" -p 1:"2026-09-13:Beacon Harbor lights" \
        -p 2:"2026-09-14:alpha omega" "$td/j.db@joint:a:s" >/dev/null 2>&1
out=$("$corm" -g . "$td/j.db:a:s" 2>/dev/null)
assert_eq "joint-seed-refs" "1
2" "$out"
"$corm" -d 2 "$td/j.db@joint:a:s" >/dev/null 2>&1
assert_ok "joint-forget-exit" "0" "$?"
out=$("$corm" -g . "$td/j.db:a:s" 2>/dev/null)
assert_eq "joint-forget-persisted" "1" "$out"

echo "=== F4a: roster pre-exists, then seed → refs must persist ==="
"$corm" --list-axes "$td/m.db@joint,stoma:a:s" >/dev/null 2>&1
"$corm" -p 1:"2026-09-13:Beacon Harbor lights" \
        -p 2:"2026-09-14:alpha omega" \
        -p 3:"2026-09-15:beacon beacon harbor" \
        "$td/m.db@joint,stoma:a:s" >/dev/null 2>&1
out=$("$corm" -g . "$td/m.db:a:s" 2>/dev/null)
assert_eq "seed-persisted" "1
2
3" "$out"

echo "=== composed search (joint window AND stoma) ==="
expr='(joint AND stoma)'
out=$("$corm" -X "$expr" -g . --since=2026-09-14 --until=2026-09-16 \
	--query=beacon --field=text --matched=1 "$td/m.db:a:s" -t 10 2>/dev/null)
assert_eq "composed-and" "1 0.166667 2026-09-13:Beacon Harbor lights
3 0.166667 2026-09-15:beacon beacon harbor" "$out"

echo "=== composed search without -g .: implicit query ==="
out=$("$corm" -X "$expr" --since=2026-09-14 --until=2026-09-16 \
	--query=beacon --field=text --matched=1 "$td/m.db:a:s" -t 10 2>/dev/null)
assert_eq "composed-and-implicit" "1 0.166667 2026-09-13:Beacon Harbor lights
3 0.166667 2026-09-15:beacon beacon harbor" "$out"

echo "=== pure-filter joint-only search (no score columns) ==="
out=$("$corm" -X 'joint' -g . --since=0 --until=2026-09-14 "$td/m.db:a:s" 2>/dev/null)
assert_eq "joint-before" "1 2026-09-13:Beacon Harbor lights" "$out"

echo "=== F4b: forget with joint+stoma roster → gone from all three ==="
"$corm" -d 3 "$td/m.db@joint,stoma:a:s" >/dev/null 2>&1
assert_ok "forget-exit" "0" "$?"
"$corm" -d 3 "$td/m.db@joint,stoma:a:s" >/dev/null 2>&1
assert_ok "forget-idempotent" "0" "$?"
out=$("$corm" -g . "$td/m.db:a:s" 2>/dev/null)
assert_eq "forget-primary" "1
2" "$out"
out=$("$corm" -X 'stoma' -g . --query=beacon --field=text --matched=1 "$td/m.db:a:s" 2>/dev/null)
assert_eq "forget-stoma" "1 0.166667 2026-09-13:Beacon Harbor lights" "$out"
out=$("$corm" -X 'joint' -g . --since=2026-09-15 --until=2026-09-16 "$td/m.db:a:s" 2>/dev/null)
# Presence semantics (F2): refs 1/2 own open [date,inf) intervals that still
# overlap the window; only the forgotten ref 3 must be gone.
assert_eq "forget-joint" "1 2026-09-13:Beacon Harbor lights
2 2026-09-14:alpha omega" "$out"

echo "=== reset: documented enumerate+forget loop ==="
"$corm" -p 1:"2026-09-13:Beacon Harbor lights" \
        -p 2:"2026-09-14:alpha omega" \
        -p 3:"2026-09-15:beacon beacon harbor" \
        "$td/r.db@joint,stoma:a:s" >/dev/null 2>&1
for ref in $("$corm" -g . "$td/r.db:a:s" 2>/dev/null | grep -E '^[0-9]+$'); do
	"$corm" -d "$ref" "$td/r.db@joint,stoma:a:s" >/dev/null 2>&1
done
assert_ok "reset-exit" "0" "$?"
out=$("$corm" -g . "$td/r.db:a:s" 2>/dev/null)
assert_eq "reset-empty" "-1" "$out"
out=$("$corm" -X 'stoma' -g . --query=beacon --field=text --matched=1 "$td/r.db:a:s" 2>/dev/null)
assert_eq "reset-stoma-empty" "" "$out"
out=$("$corm" -X 'joint' -g . --since=0 --until=2026-10-01 "$td/r.db:a:s" 2>/dev/null)
assert_eq "reset-joint-empty" "" "$out"
echo "=== reset is idempotent ==="
for ref in $("$corm" -g . "$td/r.db:a:s" 2>/dev/null | grep -E '^[0-9]+$'); do
	"$corm" -d "$ref" "$td/r.db@joint,stoma:a:s" >/dev/null 2>&1
done
assert_ok "reset-again" "0" "$?"
out=$("$corm" -g . "$td/r.db:a:s" 2>/dev/null)
assert_eq "reset-again-empty" "-1" "$out"

echo "=== classic zero-plugin regression (no roster anywhere) ==="
"$corm" -p 5:q "$td/fresh.db:a:s" >/dev/null 2>&1
(
	unset CORM_AXIS_PATH
	unset CORM_AXIS_LIBS
	out=$("$corm" -g . "$td/fresh.db:a:s" 2>/dev/null)
	assert_eq "classic-all" "5" "$out"
	out=$("$corm" -g . "$td/never.db:a:s" 2>/dev/null)
	assert_eq "classic-empty-sentinel" "-1" "$out"
)

if [ "$fail" = "0" ]; then
	echo "test-mm.sh: all green"
else
	echo "test-mm.sh: $fail FAIL(s)"
	exit 1
fi
