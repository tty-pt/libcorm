#!/bin/sh -e
# test-cli.sh — end-to-end test for the 2B-3 composed -X/-g . fold
# (mm-plan/2B-3-IMPLEMENTATION.md §7). Exercises the whole composed path:
# -X set-expression parsing (AND/OR/NOT/parens/prefix-NOT), D9 by-name +
# env-lib axis loading, composed render (ref + score + record), pure-filter,
# --top/--bottom, --list-axes (with @ roster and standalone), dangling-ref
# skip, and the classic flat CLI regression — against the functional
# lib/librec_axis_fold.so plugin (alpha/beta/pure over real corm a:u stores).
# test-roster.sh (2B-1 sidecar behavior) still passes independently.

td=$(mktemp -d)
trap 'rm -rf "$td"' EXIT

export LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH
corm=./bin/corm
fold=./lib/librec_axis_fold.so

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

assert_fails() {
	name=$1
	expects=$2
	shift 2
	if "$corm" "$@" >/dev/null 2>"$td/stderr"; then
		echo "FAIL - $name (expected nonzero exit)"
		fail=1
	elif grep -q "$expects" "$td/stderr"; then
		echo "ok - $name"
	else
		echo "FAIL - $name (stderr lacks '$expects')"
		echo "  stderr: $(cat "$td/stderr")"
		fail=1
	fi
}

echo "=== seeding (envs clean) ==="
# Axis fills are per-primary (7-AXIS-NAMESPACE-PLAN.md): each is seeded as
# the primary-owned alongside store <primary>-<axis>, e.g. demo.db-alpha.
"$corm" -p 1:one -p 2:two -p 3:three -p 4:four "$td/demo.db:a:s" >/dev/null
"$corm" -p 1:r "$td/roster.db:a:s" >/dev/null
"$corm" -p 1:1 -p 2:2 -p 3:3 "$td/demo.db-alpha:a:u" >/dev/null
"$corm" -p 2:2 -p 3:3 -p 4:4 "$td/demo.db-beta:a:u" >/dev/null
"$corm" -p 1:1 -p 2:2 "$td/demo.db-pure:a:u" >/dev/null

export CORM_AXIS_LIBS=$PWD/$fold
export CORM_AXIS_PATH=./lib

echo "=== --list-axes with @ roster ==="
out=$("$corm" --list-axes "$td/roster.db@alpha,beta,pure:a:s")
expected="slot  name              fill  rank  ctx
0     alpha             y     y     y
1     beta              y     y     y
2     pure              y     n     y"
assert_eq "list-axes-with-at" "$expected" "$out"

echo "=== --list-axes standalone (no file) ==="
out=$("$corm" --list-axes)
expected="slot  name              fill  rank  ctx
0     alpha             y     y     n
1     beta              y     y     n
2     pure              y     n     n"
assert_eq "list-axes-standalone" "$expected" "$out"

echo "=== -X bare name: alpha {1,2,3} ==="
out=$("$corm" -X alpha -g . "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two
1 1.000000 one"
assert_eq "bare-name" "$expected" "$out"

echo "=== -X AND: alpha AND beta = {2,3} ==="
out=$("$corm" -X "alpha AND beta" -g . "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two"
assert_eq "and" "$expected" "$out"

echo "=== -X OR: alpha OR beta = {1,2,3,4} ==="
out=$("$corm" -X "alpha OR beta" -g . "$td/demo.db:a:s")
expected="4 4.000000 four
3 3.000000 three
2 2.000000 two
1 1.000000 one"
assert_eq "or" "$expected" "$out"

echo "=== -X EXCEPT (setminus): alpha EXCEPT beta = {1} ==="
out=$("$corm" -X "alpha EXCEPT beta" -g . "$td/demo.db:a:s")
expected="1 1.000000 one"
assert_eq "except" "$expected" "$out"

echo "=== -X EXCEPT chains left-to-right: (alpha EXCEPT beta) EXCEPT pure = {} ==="
out=$("$corm" -X "alpha EXCEPT beta EXCEPT pure" -g . "$td/demo.db:a:s")
expected=""
assert_eq "except-chain" "$expected" "$out"

echo "=== -X prefix NOT: complement vs primary universe = {4} ==="
out=$("$corm" -X "NOT alpha" -g . "$td/demo.db:a:s")
expected="4 4.000000 four"
assert_eq "not-prefix" "$expected" "$out"

echo "=== -X parens + precedence: (alpha OR beta) AND pure = {1,2} ==="
out=$("$corm" -X "(alpha OR beta) AND pure" -g . "$td/demo.db:a:s")
expected="2 2.000000 two
1 1.000000 one"
assert_eq "parens-and" "$expected" "$out"

echo "=== -X precendence: alpha OR (beta AND pure) = {1,2,3} ==="
out=$("$corm" -X "alpha OR (beta AND pure)" -g . "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two
1 1.000000 one"
assert_eq "parens-or" "$expected" "$out"

echo "=== -X --top caps ==="
out=$("$corm" -X "alpha OR beta" -g . "$td/demo.db:a:s" -t 2)
expected="4 4.000000 four
3 3.000000 three"
assert_eq "top-cap" "$expected" "$out"

echo "=== -X --bottom floors ==="
out=$("$corm" -X "alpha OR beta" -g . "$td/demo.db:a:s" -b 2.5)
expected="4 4.000000 four
3 3.000000 three"
assert_eq "bottom-floor" "$expected" "$out"

echo "=== -X pure filter (no rank fn): asc refs, no score ==="
out=$("$corm" -X pure -g . "$td/demo.db:a:s")
expected="1 one
2 two"
assert_eq "pure-filter" "$expected" "$out"

echo "=== -X NAME=VALUE is gone: a loud failure pointing at flags ==="
assert_fails "name-value-removed" "no longer allowed in -X" \
	-X "alpha=hello world" -g . "$td/demo.db:a:s"

echo "=== -X empty string: unarmed, classic -g . ==="
# NOTE: classic -g . on an :a: primary prints refs (verified byte-identical
# against the pre-fold binary) — these rows pin the classic path unchanged.
out=$("$corm" -X "" -g . "$td/demo.db:a:s")
expected="1
2
3
4"
assert_eq "unarmed-empty" "$expected" "$out"

echo "=== no -X: classic -g . ==="
out=$("$corm" -g . "$td/demo.db:a:s")
expected="1
2
3
4"
assert_eq "classic-dot" "$expected" "$out"

echo "=== classic -g 1 ==="
out=$("$corm" -g 1 "$td/demo.db:a:s")
assert_eq "classic-get" "-1" "$out"

echo "=== armed -X ignores -k ==="
out=$("$corm" -k -X alpha -g . "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two
1 1.000000 one"
assert_eq "ignore-k" "$expected" "$out"

echo "=== ops interleave: composed get then put ==="
out=$("$corm" -X alpha -g . "$td/demo.db:a:s" -p 9:nine "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two
1 1.000000 one
9"
assert_eq "interleave" "$expected" "$out"

echo "=== -X needs no -g .: implicit query runs after all ops ==="
# Scratch filespecs keep demo.db byte-identical for the tail smoke test.
"$corm" -p 1:one -p 2:two "$td/impl.db:a:s" >/dev/null
"$corm" -p 1:1 -p 2:2 "$td/impl.db-alpha:a:u" >/dev/null
out=$("$corm" -X alpha "$td/impl.db:a:s")
expected="2 2.000000 two
1 1.000000 one"
assert_eq "implicit-dot-out" "$expected" "$out"

echo "=== -X no -g .: write-then-query sees the new ref ==="
"$corm" -p 1:one -p 2:two "$td/wtq.db:a:s" >/dev/null
"$corm" -p 1:1 -p 2:2 "$td/wtq.db-pure:a:u" >/dev/null
out=$("$corm" -X pure -p 3:three "$td/wtq.db@pure:a:s")
expected="3
1 one
2 two
3 three"
assert_eq "implicit-write-then-query" "$expected" "$out"

echo "=== -X no -g .: classic -g KEY coexists, query still runs ==="
# -g 1 on an :a: primary is a string-key miss (classic "-1", pinned above);
# the point is the implicit query still fires after the classic get.
out=$("$corm" -X pure -g 1 "$td/wtq.db:a:s")
expected="-1
1 one
2 two
3 three"
assert_eq "implicit-coexist-get" "$expected" "$out"

echo "=== -X empty string without -g .: unarmed, quiet ==="
out=$("$corm" -X "  " "$td/impl.db:a:s")
assert_eq "implicit-unarmed" "" "$out"

echo "=== -X unknown axis name: named error, exit 1 ==="
assert_fails "unknown-axis" "unknown axis name" -X wonka -g . "$td/demo.db:a:s"

echo "=== -X malformed expr (unclosed paren): named parse error, exit 1 ==="
assert_fails "bad-expr" "expected ')'" -X "(alpha OR beta" -g . "$td/demo.db:a:s"

echo "=== -X infix NOT is a parse error (hint: use EXCEPT), exit 1 ==="
assert_fails "infix-not" "use EXCEPT" -X "alpha NOT beta" -g . "$td/demo.db:a:s"

echo "=== -X empty parens: named parse error, exit 1 ==="
assert_fails "empty-parens" "expected expression" -X "()" -g . "$td/demo.db:a:s"

echo "=== -t invalid value ==="
assert_fails "bad-top" "invalid --top value" -t abc -g 1 "$td/demo.db:a:s"

echo "=== -b invalid value ==="
assert_fails "bad-bottom" "invalid --bottom value" -b abc -g 1 "$td/demo.db:a:s"

echo "=== -X --top 0 = all (same as 4 lines) ==="
out=$("$corm" -X "alpha OR beta" -g . "$td/demo.db:a:s" -t 0)
expected="4 4.000000 four
3 3.000000 three
2 2.000000 two
1 1.000000 one"
assert_eq "top-zero" "$expected" "$out"

echo "=== plugin CLI option broadcast: alpha AND beta both receive --query ==="
out=$("$corm" -X "alpha AND beta" -g . --query=hello "$td/demo.db:a:s" 2>"$td/cli-err")
expected="3 3.000000 three
2 2.000000 two"
assert_eq "cli-query-broadcast-out" "$expected" "$out"
if grep -q "fold alpha query=hello verbose=0" "$td/cli-err" \
		&& grep -q "fold beta query=hello verbose=0" "$td/cli-err"; then
	echo "ok - cli-query-broadcast-err"
else
	echo "FAIL - cli-query-broadcast-err"
	echo "  stderr: $(cat "$td/cli-err")"
	fail=1
fi

echo "=== plugin option value reaches fill: last occurrence wins ==="
out=$("$corm" -X alpha -g . --query=one --query=two "$td/demo.db:a:s" 2>"$td/cli-err2")
if grep -q "fold alpha query=two verbose=0" "$td/cli-err2"; then
	echo "ok - cli-query-last-wins"
else
	echo "FAIL - cli-query-last-wins"
	echo "  stderr: $(cat "$td/cli-err2")"
	fail=1
fi

echo "=== plugin option with no --query run: no broadcast side-channel ==="
out=$("$corm" -X alpha -g . "$td/demo.db:a:s" 2>"$td/cli-err3")
if ! grep -q "fold " "$td/cli-err3"; then
	echo "ok - cli-no-opt-silent"
else
	echo "FAIL - cli-no-opt-silent"
	echo "  stderr: $(cat "$td/cli-err3")"
	fail=1
fi

echo "=== bare flag --verbose accepted ==="
out=$("$corm" -X alpha -g . --verbose "$td/demo.db:a:s" 2>"$td/cli-err4")
if grep -q "fold alpha query=(null) verbose=1" "$td/cli-err4"; then
	echo "ok - cli-bare-flag"
else
	echo "FAIL - cli-bare-flag"
	echo "  stderr: $(cat "$td/cli-err4")"
	fail=1
fi

echo "=== unknown long option: named error, exit 1 ==="
assert_fails "cli-unknown-opt" "unknown option '--nope'" -X alpha -g . --nope=1 "$td/demo.db:a:s"

echo "=== bare --query (no =): requires --query=VALUE, exit 1 ==="
assert_fails "cli-bare-query" "requires --query=VALUE" -X alpha -g . --query "$td/demo.db:a:s"

echo "=== --verbose=1 on a bare-flag option: takes no value, exit 1 ==="
assert_fails "cli-verbose-val" "takes no value" -X alpha -g . --verbose=1 "$td/demo.db:a:s"

echo "=== D15 labeled instances: (A:alpha AND B:beta) with scoped flags ==="
# Per-instance delivery is observable via the fold leaf= echo; the plain
# broadcast claim (query=... verbose=0) must be ABSENT for scoped tokens.
out=$("$corm" -X '(A:alpha AND B:beta)' -g . --query@A=hello --query@B=world \
	"$td/demo.db:a:s" 2>"$td/d15-err")
expected="3 3.000000 three
2 2.000000 two"
assert_eq "d15-scoped-out" "$expected" "$out"
if grep -q "fold alpha leaf=query=hello" "$td/d15-err" \
		&& grep -q "fold beta leaf=query=world" "$td/d15-err" \
		&& ! grep -q "query=hello verbose" "$td/d15-err"; then
	echo "ok - d15-scoped-err"
else
	echo "FAIL - d15-scoped-err"
	echo "  stderr: $(cat "$td/d15-err")"
	fail=1
fi

echo "=== E_REF backward-only references ==="
out=$("$corm" -X 'A:alpha AND A' -g . "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two
1 1.000000 one"
assert_eq "d15-eref-self" "$expected" "$out"

out=$("$corm" -X 'NOT A:alpha' -g . "$td/demo.db:a:s")
expected="9 9.000000 nine
4 4.000000 four"
assert_eq "d15-eref-not" "$expected" "$out"

out=$("$corm" -X 'B:beta AND B' -g . "$td/demo.db:a:s")
expected="4 4.000000 four
3 3.000000 three
2 2.000000 two"
assert_eq "d15-eref-later" "$expected" "$out"

echo "=== --rank@A picks the labeled instance ==="
out=$("$corm" -X '(A:alpha AND B:beta)' -g . --rank@B "$td/demo.db:a:s")
expected="3 3.000000 three
2 2.000000 two"
assert_eq "d15-rank-b" "$expected" "$out"

out=$("$corm" -X '(A:alpha AND B:beta)' -g . --rank@B -t 1 "$td/demo.db:a:s")
expected="3 3.000000 three"
assert_eq "d15-rank-b-top1" "$expected" "$out"

echo "=== D15 grammar/scope error surface (exit 1, exact stderr) ==="
assert_fails "d15-dup-label" "duplicate label 'A'" -X 'A:alpha EXCEPT A:alpha' -g . "$td/demo.db:a:s"
assert_fails "d15-forward-ref" "unknown axis name 'A'" -X 'A AND A:alpha' -g . "$td/demo.db:a:s"
assert_fails "d15-label-collision" "label 'alpha' collides with axis name 'alpha'" -X 'alpha:alpha AND beta' -g . "$td/demo.db:a:s"
assert_fails "d15-scope-unknown" "unknown label or axis 'Nope'" -X 'A:alpha' -g . --query@Nope=hello "$td/demo.db:a:s"
assert_fails "d15-scope-no-value" "requires --query@A=VALUE" -X 'A:alpha' -g . --query@A "$td/demo.db:a:s"
assert_fails "d15-scope-bare-flag" "takes no value" -X 'A:alpha' -g . --verbose@A "$td/demo.db:a:s"

echo "=== >16 plugin options: too-many error, exit 1 ==="
many=""
i=0
while [ "$i" -lt 17 ]; do many="$many --nope$i=1"; i=$((i + 1)); done
assert_fails "cli-too-many" "too many plugin options" -X alpha -g . $many "$td/demo.db:a:s"

echo "=== -? still prints usage, exit 0 ==="
if "$corm" -? >/dev/null 2>"$td/help-err"; then
	if grep -q "Usage:" "$td/help-err"; then
		echo "ok - dash-q-help"
	else
		echo "FAIL - dash-q-help (usage missing)"
		fail=1
	fi
else
	echo "FAIL - dash-q-help (expected exit 0)"
	fail=1
fi

echo "=== -Z still unknown short: usage, exit 0 (unchanged) ==="
if "$corm" -Z "$td/demo.db:a:s" >/dev/null 2>"$td/help-err2"; then
	echo "ok - dash-Z-help"
else
	echo "FAIL - dash-Z-help (expected exit 0)"
	fail=1
fi

echo "=== classic smoke with envs unset ==="
# NOTE: the interleave row above persisted ref 9 into demo.db, so the
# classic listing shows 5 refs. Pins the classic path byte-identical.
unset CORM_AXIS_LIBS CORM_AXIS_PATH
out=$("$corm" -g . "$td/demo.db:a:s")
expected="1
2
3
4
9"
assert_eq "classic-smoke" "$expected" "$out"

if [ "$fail" -ne 0 ]; then
	echo "test-cli.sh: FAILURES ABOVE" >&2
	exit 1
fi
echo "test-cli.sh: all green"