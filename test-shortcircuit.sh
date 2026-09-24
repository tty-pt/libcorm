#!/bin/sh -e
# test-shortcircuit.sh — L1 short-circuit gate for corm_expr_eval
# (AXIS-EFF plan L1): once an E_AND/E_SUB running set is empty (∅∩X=∅,
# ∅∖X=∅) the remaining branches are skipped WITHOUT decoding their leaves
# — so e.g. a (text AND sepal) scan pays no sepal embed HTTP call when the
# text branch already empties the conjunction.
#
# Probe axis: lib/librec_axis_probe.so ("pe") — decode(value) emits
# "AXIS-BOOM-DECODED" on stderr for the value "boom"; fill() returns the
# empty set for the value "empty", else refs {1,2,3}. Result-exactness is
# also pinned: skipping never changes the printed rows.
#
# Post-flip grammar is structure-only: per-instance values ride scoped
# flags (--query@A=empty), delivered to the bare leaf's decode via the
# probe CLI fallback. Two differently-valued bare `pe` leaves therefore
# enumerate as labeled instances A/B.

td=$(mktemp -d)
trap 'rm -rf "$td"' EXIT

export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
corm=./bin/corm
probe=./lib/librec_axis_probe.so

fail=0

marker_grep() {
	# usage: marker_grep <name> <want(0=absent,1=present)> [args...]
	name=$1
	want=$2
	shift 2
	if "$corm" "$@" >"$td/out" 2>"$td/err"; then
		rc=0
	else
		rc=$?
	fi
	if [ "$rc" -ne 0 ]; then
		echo "FAIL - $name (corm exitted $rc)"
		fail=1
		return
	fi
	if grep -q 'AXIS-BOOM-DECODED' "$td/err"; then
		got=1
	else
		got=0
	fi
	if [ "$want" -eq "$got" ]; then
		echo "ok - $name"
	else
		echo "FAIL - $name (marker want=$want got=$got)"
		cat "$td/err"
		fail=1
	fi
}

echo "=== seeding probe.db (refs 1..3) ==="
"$corm" -p 1:a -p 2:b -p 3:c "$td/probe.db:a:s" >/dev/null

export CORM_AXIS_LIBS=$PWD/$probe
export CORM_AXIS_PATH=./lib
filespec="$td/probe.db@pe:a:s"

echo "=== AND: empty first → boom branch skipped (no decode marker) ==="
marker_grep "and-empty-first-skips" 0 \
	-X '(A:pe AND B:pe)' -g . --query@A=empty --query@B=boom "$filespec"

echo "=== AND control: boom first → leaf still decodes ==="
marker_grep "and-control-boom-first-decodes" 1 \
	-X '(A:pe AND B:pe)' -g . --query@A=boom --query@B=empty "$filespec"

echo "=== EXCEPT: empty left → right skipped ==="
marker_grep "except-empty-left-skips" 0 \
	-X '(A:pe EXCEPT B:pe)' -g . --query@A=empty --query@B=boom "$filespec"

echo "=== EXCEPT control: non-empty left → right decodes ==="
marker_grep "except-control-right-decodes" 1 \
	-X '(A:pe EXCEPT B:pe)' -g . --query@A=zhit --query@B=boom "$filespec"

echo "=== result-exactness: empty AND boom prints no rows, exit 0 ==="
out=$("$corm" -X '(A:pe AND B:pe)' -g . --query@A=empty --query@B=boom "$filespec" 2>/dev/null)
if [ -z "$out" ]; then
	echo "ok - empty-and result empty"
else
	echo "FAIL - empty-and result not empty: $out"
	fail=1
fi

echo "=== result-exactness: plain zhit AND zhit still rows 1..3 ==="
out=$("$corm" -X '(A:pe AND B:pe)' -g . --query@A=zhit --query@B=zhit "$filespec")
expected="1 a
2 b
3 c"
if [ "$out" = "$expected" ]; then
	echo "ok - zhit-and rows intact"
else
	echo "FAIL - zhit-and rows changed"
	echo "  expected: $(printf '%s' "$expected" | tr '\n' '|')"
	echo "  actual:   $(printf '%s' "$out" | tr '\n' '|')"
	fail=1
fi

echo "=== D15: labeled same-axis instances short-circuit independently ==="
marker_grep "d15-and-empty-first-skips" 0 \
	-X '(A:pe AND B:pe)' -g . --query@A=empty --query@B=boom "$filespec"

marker_grep "d15-and-control-boom-first-decodes" 1 \
	-X '(A:pe AND B:pe)' -g . --query@A=boom --query@B=empty "$filespec"

echo "=== D15: E_REF with empty instance in EXCEPT ==="
out=$("$corm" -X '(A:pe EXCEPT A)' -g . --query@A=empty "$filespec" 2>/dev/null)
if [ -z "$out" ]; then
	echo "ok - d15-eref-except-empty"
else
	echo "FAIL - d15-eref-except not empty: $out"
	fail=1
fi

out=$("$corm" -X '(A:pe EXCEPT B:pe)' -g . --query@A=zhit --query@B=empty "$filespec")
expected="1 a
2 b
3 c"
if [ "$out" = "$expected" ]; then
	echo "ok - d15-two-instances-except"
else
	echo "FAIL - d15-two-instances-except rows changed"
	echo "  expected: $(printf '%s' "$expected" | tr '\n' '|')"
	echo "  actual:   $(printf '%s' "$out" | tr '\n' '|')"
	fail=1
fi

exit $fail