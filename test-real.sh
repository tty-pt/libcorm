#!/bin/sh -e
# test-real.sh — 2B-2 real-file gate (mm-plan/PHASE-2-CLI.md 2B-2).
#
# Non-mm space∩time∩text over REAL primary+joint+islet+sepal files with the
# SAME refs (1/2/3), stoma rebuilt from the primary at each open (never
# files). Also asserts: plain -g answers with zero plugins on a fresh
# primary, sepal offline floats-direct (the string→embed→ANN column is
# skipped unless CORM_SEPAL_EMBED_URL+MODEL are both set — D8), the
# stoma rebuild budget note is measured + recorded (mm-plan U4), and the
# 2B-6 D2 rank pin (first rank-capable leaf in preorder wins when two
# rankers share one -X expression: stoma over sepal cosine).
#
# Requires the four real axis .so's; resolved from $CORM_AXIS_REAL_LIBS
# (colon list, default = the in-site submodule builds under
# external/lib{joint,islet,sepal,stoma}/lib, relative to this script).
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

assert_contains() {
	name=$1
	needle=$2
	haystack=$3
	if printf '%s' "$haystack" | grep -q "$needle"; then
		echo "ok - $name"
	else
		echo "FAIL - $name (missing '$needle')"
		echo "  haystack: $(printf '%s' "$haystack" | tr '\n' '|')"
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
# Default: the in-site submodule builds, relative to this script (explicit
# $CORM_AXIS_REAL_LIBS still overrides).
script_dir=$(dirname "$0")
real_libs=${CORM_AXIS_REAL_LIBS:-$script_dir/../libjoint/lib:$script_dir/../libislet/lib:$script_dir/../libsepal/lib:$script_dir/../libstoma/lib}
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

echo "=== preflight: real axis .so ==="
missing=0
for a in joint islet sepal stoma; do
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
	echo "test-real.sh: REAL AXIS LIBRARIES MISSING (1 FAIL)"
	exit 1
fi

# --- compile the seeder against the real libs ---
echo "=== compile seeder ==="
cc -o "$td/real_seed" tests/real_seed.c -Iinclude -ldl 2>"$td/cc.err" \
	|| { cat "$td/cc.err"; echo "FAIL - seeder compile"; exit 1; }
echo "ok - seeder compiled"

echo "=== seeding (explicit refs 1/2/3 into primary + axes) ==="
"$corm" -p 1:"2026-09-13T20:00:00:Beacon Harbor lights" \
        -p 2:"2026-09-15T08:00:00:alpha omega" \
        -p 3:"2026-09-14T12:00:00:Beacon Harbor lights" \
        "$td/greps.db:a:s" >/dev/null
"$corm" -p 1:plain -p 2:records "$td/fresh.db:a:s" >/dev/null
"$td/real_seed" "$axis_path" "$td/greps.db" 2>"$td/seed.err" || { cat "$td/seed.err"; echo "FAIL - seeding"; exit 1; }
echo "ok - seeded"

qvec="$td/q.vec"
qdim=$(cat "$td/q.dim")
if [ "$qdim" -ge 1 ] 2>/dev/null; then
	echo "ok - qvec dim $qdim"
else
	echo "FAIL - bad q.dim: $qdim"
	fail=1
fi

echo "=== embed column ==="
if [ -n "$CORM_SEPAL_EMBED_URL" ] && [ -n "$CORM_SEPAL_EMBED_MODEL" ]; then
	echo "embed mode (CORM_SEPAL_EMBED_URL+MODEL set)"
else
	echo "ok - floats-direct column (offline default; embed skipped)"
fi

roster="$td/greps.db@joint,islet,sepal,stoma:a:s"

echo "=== first @ invocation: loud, never silent ==="
out=$("$corm" --list-axes "$roster" 2>"$td/list1.err")
echo "--- prime stderr ---"; cat "$td/list1.err"
# The roster sidecar is exit-flushed, so the first-ever @ invocation cannot
# see it: the CLI says so (alongside-heuristic) and stoma says so too.
# Both notes prove the empty-index path is never silent.
if grep -q "but no roster yet" "$td/list1.err"; then
	echo "ok - prime-alongside-hint"
else
	echo "FAIL - prime-alongside-hint (missing 'but no roster yet')"
	echo "  stderr: $(cat "$td/list1.err" | tr '\n' '|')"
	fail=1
fi
if grep -q "no primary found" "$td/list1.err"; then
	echo "ok - prime-stoma-loud-empty"
else
	echo "FAIL - prime-stoma-loud-empty (missing 'no primary found')"
	echo "  stderr: $(cat "$td/list1.err" | tr '\n' '|')"
	fail=1
fi

echo "=== --list-axes with @ roster (all four real axes bound) ==="
out=$("$corm" --list-axes "$roster" 2>"$td/list2.err")
echo "--- bound stderr ---"; cat "$td/list2.err"
expected="slot  name              fill  rank  ctx
0     joint             y     n     y
1     islet             y     n     y
2     sepal             y     y     y
3     stoma             y     y     y"
assert_eq "list-axes-real" "$expected" "$out"
# The rebuild budget note (docs + ms) — proves rebuild runs at each open.
if grep -q "rebuilt 3 docs in" "$td/list2.err"; then
	echo "ok - budget-note"
else
	echo "FAIL - budget-note (missing 'rebuilt 3 docs in')"
	echo "  stderr: $(cat "$td/list2.err" | tr '\n' '|')"
	fail=1
fi

echo "=== space ∩ time ∩ text: the conjunctive winner ==="
# Post-flip structure-only -X; every parameter rides scoped flags.
expr='(A:joint AND B:islet) AND C:stoma'
out=$("$corm" -X "$expr" -g . --since@A=2026-09-14 --until@A=2026-09-15 \
	--dim@B=2 --s@B=9,1 --l@B=1,1 --query@C=beacon --field@C=text \
	--matched@C=1 -t 100 "$roster" 2>"$td/q.err")
echo "--- query stderr ---"; cat "$td/q.err"
echo "--- query stdout ---"; printf '%s\n' "$out"
expected="3 0.125000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "winner" "$expected" "$out"

echo "=== sepal column query ==="
out=$("$corm" -X 'sepal' -g . --file="$qvec" --qdim="$qdim" --m=2 \
	--min-sim=0.5 -t 100 "$roster" 2>"$td/s.err")
echo "--- sepal stderr ---"; cat "$td/s.err"
echo "--- sepal stdout ---"; printf '%s\n' "$out"
if [ -n "$CORM_SEPAL_EMBED_URL" ] && [ -n "$CORM_SEPAL_EMBED_MODEL" ]; then
	# Env-gated embed column (ANN over embedded strings): the values are
	# endpoint-produced, so this case stays structural — exit 0, the hook
	# configured cleanly (no inconsistent-pair warning), and the query
	# answered with at least one row.
	if grep -q "inconsistent" "$td/s.err"; then
		echo "FAIL - sepal-embed-configured (hook rejected the pair)"
		cat "$td/s.err"
		fail=1
	else
		echo "ok - sepal-embed-configured"
	fi
	if printf '%s' "$out" | grep -q .; then
		echo "ok - sepal-embed-nonempty"
	else
		echo "FAIL - sepal-embed-nonempty (no rows)"
		fail=1
	fi
else
	expected="3 1.000000 2026-09-14T12:00:00:Beacon Harbor lights
1 0.906867 2026-09-13T20:00:00:Beacon Harbor lights"
	assert_eq "sepal-floats" "$expected" "$out"

	echo "=== scoped --min-sim@A over sepal: parity with unscoped ==="
	# D15 scoped synth emits min-sim='…' as a leaf decode key; sepal decode
	# must map it (regression: decode only matched min_sim, so a scoped
	# threshold was silently ignored). 0.95 excludes ref 1 (0.9069) but not
	# ref 3 (1.0) — a floor that only a WORKING scoped min-sim can strip.
	out=$("$corm" -X 'A:sepal' -g . --file="$qvec" --qdim="$qdim" \
		--m=2 --min-sim@A=0.95 -t 100 "$roster" 2>"$td/s3.err")
	echo "--- scoped min-sim stderr ---"; cat "$td/s3.err"
	echo "--- scoped min-sim stdout ---"; printf '%s\n' "$out"
	expected="3 1.000000 2026-09-14T12:00:00:Beacon Harbor lights"
	assert_eq "sepal-scoped-min-sim" "$expected" "$out"

	echo "=== two rankers, one expression: first rank-capable leaf in preorder wins (D2) ==="
	# stoma query is scoped (its field/query would otherwise collide on the
	# shared broadcast --query); sepal takes unscoped file/qdim/m/min-sim.
	sexpr2='C:stoma AND sepal'
	out=$("$corm" -X "$sexpr2" -g . --query@C=beacon --field@C=text \
		--matched@C=1 --file="$qvec" --qdim="$qdim" --m=2 --min-sim=0.5 \
		-t 100 "$roster" 2>"$td/s2.err")
	echo "--- two-rank stderr ---"; cat "$td/s2.err"
	echo "--- two-rank stdout ---"; printf '%s\n' "$out"
	# First-in-preorder stoma ranks (matched/doc-tokens 0.125 both), NOT
	# sepal cosine (which would print "3 1.000000" / "1 0.906867"). Tie →
	# asc ref.
	expected="1 0.125000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.125000 2026-09-14T12:00:00:Beacon Harbor lights"
 	assert_eq "stoma-over-sepal" "$expected" "$out"
fi

echo "=== sepal column without -g .: implicit query, same rows ==="
# Env-proof parity: whatever the with-dot run answers (floats or embed),
# the implicit end-run must answer byte-identical.
out_dot=$("$corm" -X 'sepal' -g . --file="$qvec" --qdim="$qdim" --m=2 \
	--min-sim=0.5 -t 100 "$roster" 2>/dev/null)
out_impl=$("$corm" -X 'sepal' --file="$qvec" --qdim="$qdim" --m=2 \
	--min-sim=0.5 -t 100 "$roster" 2>/dev/null)
assert_eq "sepal-implicit-parity" "$out_dot" "$out_impl"
if [ -z "$out_impl" ]; then
	echo "FAIL - sepal-implicit-nonempty (no rows)"
	fail=1
else
	echo "ok - sepal-implicit-nonempty"
fi

echo "=== scoped labeled instances over real axes (stoma) ==="
# D15: two stoma instances in one expression, each with its OWN scoped
# query (beacon → {1,3}, alpha → {2}). Same-axis rankers aggregate by
# max score per ref, so OR prints all three, tie → asc ref.
out=$("$corm" -X 'A:stoma OR B:stoma' -g . --query@A=beacon \
	--query@B=alpha -t 10 "$roster" 2>"$td/l1.err")
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
2 0.000000 2026-09-15T08:00:00:alpha omega
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "scoped-or" "$expected" "$out"

out=$("$corm" -X 'A:stoma EXCEPT B:stoma' -g . --query@A=beacon \
	--query@B=alpha -t 10 "$roster" 2>/dev/null)
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "scoped-except" "$expected" "$out"

out=$("$corm" -X 'NOT (A:stoma)' -g . --query@A=alpha -t 10 "$roster" 2>/dev/null)
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "scoped-not" "$expected" "$out"

echo "=== E_REF backward-only references (real stoma) ==="
out=$("$corm" -X 'A:stoma AND A' -g . --query@A=beacon -t 10 "$roster" 2>/dev/null)
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "eref-self" "$expected" "$out"

echo "=== quoted value with spaces survives the synth (stoma) ==="
out=$("$corm" -X 'A:stoma' -g . '--query@A=harbor lights' -t 10 "$roster" 2>/dev/null)
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "scoped-quoted-space" "$expected" "$out"

echo "=== scoped empty field= keeps the text default over CLI --field ==="
# Regression: an empty leaf field= must resolve to the "text" default even
# when a CLI --field is set (leaf wins, even when empty). The buggy merge
# lets the CLI "title" through → no rows (title is empty in this roster).
out=$("$corm" -X 'A:stoma' -g . --query@A=beacon --field@A= --field=title \
	-t 10 "$roster" 2>/dev/null)
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "scoped-empty-field-default" "$expected" "$out"

echo "=== scoped joint via since=/until= aliases (a=/b= in the leaf) ==="
# D15 decode alias: --since@A/--until@A synthesize since=/until= leaf
# keys, which joint_decode accepts alongside a=/b=. A ⊂ {1,3}; E_REF
# re-uses A's set (AND is a no-op); NOT moves to the complement.
out=$("$corm" -X 'A:joint AND A' -g . --since@A=2026-09-14 \
	--until@A=2026-09-15 -t 10 "$roster" 2>/dev/null)
expected="1 2026-09-13T20:00:00:Beacon Harbor lights
3 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "joint-scoped-eref" "$expected" "$out"

out=$("$corm" -X 'NOT (A:joint)' -g . --since@A=2026-09-14 \
	--until@A=2026-09-15 -t 10 "$roster" 2>/dev/null)
expected="2 2026-09-15T08:00:00:alpha omega"
assert_eq "joint-scoped-not" "$expected" "$out"

out=$("$corm" -X 'A:joint OR B:joint' -g . --since@A=2026-09-14 \
	--until@A=2026-09-15 --since@B=2026-09-15 --until@B=2026-09-16 \
	-t 10 "$roster" 2>/dev/null)
expected="1 2026-09-13T20:00:00:Beacon Harbor lights
2 2026-09-15T08:00:00:alpha omega
3 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "joint-two-scopes" "$expected" "$out"

echo "=== escape roundtrips through the real stoma tokenizer ==="
# Second primary whose text carries an apostrophe and a backslash; the
# scoped values must arrive byte-identical (synth escapes, stoma decode
# unescapes). Timestamp prefixes keep the joint column parseable.
"$corm" -p 1:"2026-09-13T00:00:00:plain record" \
	-p 2:"2026-09-14T00:00:00:don't stop thinking" \
	-p 3:"2026-09-15T00:00:00:back\\slash path" \
	"$td/esc.db:a:s" >/dev/null
"$td/real_seed" "$axis_path" "$td/esc.db" 2>"$td/esc-seed.err" \
	|| { cat "$td/esc-seed.err"; echo "FAIL - esc seeding"; fail=1; }
esc_roster="$td/esc.db@joint,islet,sepal,stoma:a:s"
"$corm" --list-axes "$esc_roster" >/dev/null 2>/dev/null
out=$("$corm" -X 'A:stoma' -g . "--query@A=don't" -t 10 "$esc_roster" 2>/dev/null)
expected="2 0.000000 2026-09-14T00:00:00:don't stop thinking"
assert_eq "escape-apostrophe" "$expected" "$out"

out=$("$corm" -X 'A:stoma' -g . --query@A='back\slash' -t 10 "$esc_roster" 2>/dev/null)
expected="3 0.000000 2026-09-15T00:00:00:back\\slash path"
assert_eq "escape-backslash" "$expected" "$out"

echo "=== shared broadcast --query: joint accept-and-ignore, stoma answers ==="
# FLAGS-FIRST regression: an unscoped --query=beacon is broadcast to every
# bound axis declaring `query` — joint declares it too. joint must
# accept-and-ignore the non-parseable "beacon" (no abort, window from
# --since/--until wins) while stoma still matches. Binds only joint+stoma
# so the broadcast cannot double-feature sepal's own query path.
js_roster="$td/greps.db@joint,stoma:a:s"
out=$("$corm" -X '(joint AND stoma)' -g . --query=beacon --since=0 \
	--until=2026-09-16 -t 10 "$js_roster" 2>"$td/shared.err")
echo "--- shared stderr ---"; cat "$td/shared.err"
expected="1 0.000000 2026-09-13T20:00:00:Beacon Harbor lights
3 0.000000 2026-09-14T12:00:00:Beacon Harbor lights"
assert_eq "shared-query-regression" "$expected" "$out"

echo "=== classic zero-plugin regression (fresh, no roster) ==="
(
	unset CORM_AXIS_PATH
	unset CORM_AXIS_LIBS
	out=$("$corm" -g 1 "$td/fresh.db:a:s")
	assert_eq "classic-get-zero-plugins" "-1" "$out"
	out=$("$corm" -g . "$td/fresh.db:a:s")
	expected="1
2"
	assert_eq "classic-all-zero-plugins" "$expected" "$out"
)

if [ "$fail" = "0" ]; then
	echo "test-real.sh: all green"
else
	echo "test-real.sh: $fail FAIL(s)"
	exit 1
fi
