#!/bin/bash
# Smoke test: run a few examples interpreted and via --exec, compare to
# expected output. Exits 0 on success, nonzero on any failure.
#
# Run from the knot root directory:
#   ./scripts/smoke_test.sh

set -e
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

check() {
    local name="$1"
    local expected="$2"
    local got="$3"
    if [ "$got" = "$expected" ]; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        echo "        expected: $expected"
        echo "        got:      $got"
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x ./knot ]; then
    echo "no ./knot binary -- run \`make\` first"
    exit 1
fi

echo "== interpreter =="

GOT=$(./knot examples/bisect_annotated.knot 2>&1)
check "bisect (interp)" "1.41421" "$GOT"

GOT=$(./knot examples/tour.knot 2>&1 | tail -1)
check "tour last line (interp)" "3" "$GOT"

GOT=$(./knot examples/for_tour.knot 2>&1 | tail -1)
check "for_tour last line (interp)" "2" "$GOT"

echo "== transpiler =="

# Clear any cached binaries so we exercise the full path.
rm -f /tmp/knot_*.out /tmp/knot_*.srchash /tmp/knot_*.c

# --exec runs go through cc, which can emit warnings to stderr; drop
# those so the program's stdout is what we compare. Real --exec runtime
# errors land in --exec's own stderr (already covered by separate checks).
GOT=$(./knot --exec examples/bisect_annotated.knot 2>/dev/null)
check "bisect (--exec)" "1.41421" "$GOT"

GOT=$(./knot --exec examples/for_tour.knot 2>/dev/null | tail -1)
check "for_tour last line (--exec)" "2" "$GOT"

# Option pricer: just check that the Black-Scholes line is right.
GOT=$(./knot --exec examples/option_pricer.knot 2>/dev/null | grep -A1 "Closed-form" | tail -1)
check "option pricer BS call" "  call = 10.4506" "$GOT"

echo "== tag detection (new for syntax) =="

cat > /tmp/_swap_for.knot <<'EOF'
A = [[1, 2, 3], [4, 5, 6]]
for i to rows(A) {
    for j to cols(A) {
        print(A[j, i])
    }
}
EOF
GOT=$(./knot /tmp/_swap_for.knot 2>&1 | head -1)
check "axis swap caught (for)" "error: index came from cols(mat) but is being used as row index (index carries col count)" "$GOT"
rm -f /tmp/_swap_for.knot

echo "== tag detection (legacy loop syntax) =="

# The loop keyword is kept as a legacy form for backward compatibility.
cat > /tmp/_swap_loop.knot <<'EOF'
A = [[1, 2, 3], [4, 5, 6]]
loop rows(A) as i {
    loop cols(A) as j {
        print(A[j, i])
    }
}
EOF
GOT=$(./knot /tmp/_swap_loop.knot 2>&1 | head -1)
check "axis swap caught (loop)" "error: index came from cols(mat) but is being used as row index (index carries col count)" "$GOT"
rm -f /tmp/_swap_loop.knot

echo "== for-form coverage =="

# Each of the four for-forms produces the right output.
cat > /tmp/_forcov.knot <<'EOF'
# for i to N: 0..N-1, sum = 0+1+2+3+4 = 10
out1 = 0
for i to 5 { out1 += i }
print(out1)
# for x in v: elements, sum = 60
out2 = 0
for x in [10, 20, 30] { out2 += x }
print(out2)
# for i to len(v): index, sum = 0+1+2 = 3
out3 = 0
v = [3, 4, 5]
for i to len(v) { out3 += i }
print(out3)
# for i, x in v: both, 0*3 + 1*4 + 2*5 = 14
out4 = 0
for i, x in v { out4 += i * x }
print(out4)
EOF
GOT=$(./knot /tmp/_forcov.knot 2>&1 | tr '\n' ',')
check "for-form outputs (interp)" "10,60,3,14," "$GOT"
GOT=$(./knot --exec /tmp/_forcov.knot 2>/dev/null | tr '\n' ',')
check "for-form outputs (--exec)" "10,60,3,14," "$GOT"
rm -f /tmp/_forcov.knot /tmp/knot__forcov.*

echo "== extensions (C++ runtime) =="

# Deterministic sort.
cat > /tmp/_sort_test.knot <<'EOF'
v = [3.0, 1.0, 4.0, 1.0, 5.0, 9.0, 2.0, 6.0]
sort_vec(v)
print(v)
EOF
GOT=$(./knot /tmp/_sort_test.knot 2>&1)
check "sort_vec (interp)" "[1, 1, 2, 3, 4, 5, 6, 9]" "$GOT"
GOT=$(./knot --exec /tmp/_sort_test.knot 2>/dev/null)
check "sort_vec (--exec)" "[1, 1, 2, 3, 4, 5, 6, 9]" "$GOT"
rm -f /tmp/_sort_test.knot /tmp/knot__sort_test.*

# Seeded RNG: with same seed, both modes give the same outputs.
cat > /tmp/_rng_test.knot <<'EOF'
rng_seed(42)
print(rng_uniform())
print(rng_normal())
EOF
GOT_INTERP=$(./knot /tmp/_rng_test.knot 2>&1)
GOT_EXEC=$(./knot --exec /tmp/_rng_test.knot 2>/dev/null)
check "RNG agrees across modes (seed=42)" "$GOT_INTERP" "$GOT_EXEC"
rm -f /tmp/_rng_test.knot /tmp/knot__rng_test.*

# CSV read.
cat > /tmp/_csvdata.csv <<'EOF'
1, 2, 3
4, 5, 6
EOF
cat > /tmp/_csv_test.knot <<'EOF'
M = read_csv("/tmp/_csvdata.csv")
print(rows(M), cols(M))
print(M[1, 2])
EOF
GOT=$(./knot /tmp/_csv_test.knot 2>&1 | tr '\n' '|')
check "read_csv shape + index (interp)" "2 3|6|" "$GOT"
GOT=$(./knot --exec /tmp/_csv_test.knot 2>/dev/null | tr '\n' '|')
check "read_csv shape + index (--exec)" "2 3|6|" "$GOT"
rm -f /tmp/_csvdata.csv /tmp/_csv_test.knot /tmp/knot__csv_test.*

echo "== --record =="

cat > /tmp/_rec.knot <<'EOF'
x = 3
y = 4
z = x + y
EOF
./knot --record /tmp/_rec.knot > /dev/null 2>&1
# Snapshot after the last stmt should show all three names with correct values.
GOT=$(grep -A4 "STEP 2 " /tmp/_rec.knot.trace | tr '\n' '|')
check "record: final state captured" "STEP 2 line=3:1|  x = 3|  y = 4|  z = 7|" "$GOT"
# Header should be present.
GOT=$(head -1 /tmp/_rec.knot.trace)
check "record: trace header present" "# knot trace v1" "$GOT"
rm -f /tmp/_rec.knot /tmp/_rec.knot.trace

# CALL / RET events for the call-graph view.
cat > /tmp/_cg.knot <<'EOF'
def sq(x) { return x * x }
def add(a, b) { return a + b }
print(add(sq(3), sq(4)))
EOF
./knot --record /tmp/_cg.knot > /dev/null 2>&1
# Expect 2 CALLs to sq, 1 to add, with matching RETs.
GOT=$(grep -E "^(CALL|RET)" /tmp/_cg.knot.trace | tr '\n' '|')
check "record: CALL/RET events" "CALL sq(3) at line=3:11|RET sq -> 9|CALL sq(4) at line=3:18|RET sq -> 16|CALL add(9, 16) at line=3:7|RET add -> 25|" "$GOT"

# --callgraph reads the trace and emits parseable Graphviz.
GOT=$(./knot --callgraph /tmp/_cg.knot.trace | grep -E "^  \".*\" -> \".*\"")
EXPECTED='  "<top>" -> "add" [label="x1\n(9, 16)"];
  "<top>" -> "sq" [label="x2\n(3)"];'
check "callgraph: edges rendered" "$EXPECTED" "$GOT"
rm -f /tmp/_cg.knot /tmp/_cg.knot.trace

# --replay drops into a REPL; we drive it with scripted input.
cat > /tmp/_rep.knot <<'EOF'
x = 3
y = 4
z = x * y
EOF
./knot --record /tmp/_rep.knot > /dev/null 2>&1
# `history z` should report the one step where z was assigned. The
# prompt and output share a line under piped stdin, so we grep for the
# distinctive substring rather than anchoring at start-of-line.
GOT=$(echo 'history z
quit' | ./knot --replay /tmp/_rep.knot.trace 2>&1 | grep -o "step 2 line=3:1:  z = 12" | head -1)
check "replay: history finds the assignment" "step 2 line=3:1:  z = 12" "$GOT"

# `find 99` on a program that mutates a vec to contain 99 should jump
# to the step where 99 first appears.
cat > /tmp/_rep2.knot <<'EOF'
v = [1, 2, 3]
v[1] = 99
EOF
./knot --record /tmp/_rep2.knot > /dev/null 2>&1
GOT=$(echo 'find 99
quit' | ./knot --replay /tmp/_rep2.knot.trace 2>&1 | grep -o "jumped to step 1" | head -1)
check "replay: find jumps to the matching step" "jumped to step 1" "$GOT"
rm -f /tmp/_rep.knot /tmp/_rep.knot.trace /tmp/_rep2.knot /tmp/_rep2.knot.trace

echo "== --trap-nan =="

cat > /tmp/_nan_silent.knot <<'EOF'
x = sqrt(-1)
print(x + 0)
EOF
# Without the flag, NaN propagates silently -- baseline behavior.
GOT=$(./knot /tmp/_nan_silent.knot 2>&1)
check "no-trap: NaN propagates silently" "nan" "$GOT"

# With --trap-nan, the error fires at the sqrt call, not at the print.
GOT=$(./knot --trap-nan /tmp/_nan_silent.knot 2>&1 | head -1)
check "trap-nan: sqrt(-1) caught at sqrt" "error: --trap-nan: sqrt produced NaN (run without --trap-nan to allow non-finite values)" "$GOT"

# Overflow caught too (1e200 squared is +Inf).
cat > /tmp/_inf.knot <<'EOF'
x = 1e200
y = x * x
print(y)
EOF
GOT=$(./knot --trap-nan /tmp/_inf.knot 2>&1 | head -1)
check "trap-nan: overflow caught" "error: --trap-nan: arithmetic produced +Inf (run without --trap-nan to allow non-finite values)" "$GOT"
rm -f /tmp/_nan_silent.knot /tmp/_inf.knot

echo "== break / continue =="

cat > /tmp/_bc.knot <<'EOF'
out = 0
for i to 100 {
    if i == 5 { break }
    out += i
}
print(out)
# 0 + 1 + 2 + 3 + 4 = 10
for i to 5 {
    if i == 2 { continue }
    print(i)
}
EOF
GOT=$(./knot /tmp/_bc.knot 2>&1 | tr '\n' ',')
check "break/continue (interp)" "10,0,1,3,4," "$GOT"
GOT=$(./knot --exec /tmp/_bc.knot 2>/dev/null | tr '\n' ',')
check "break/continue (--exec)" "10,0,1,3,4," "$GOT"
rm -f /tmp/_bc.knot /tmp/knot__bc.*

# Break outside a loop should error cleanly, not crash.
cat > /tmp/_bc_err.knot <<'EOF'
x = 1
break
EOF
GOT=$(./knot /tmp/_bc_err.knot 2>&1 | head -1)
check "break outside loop errors" "error: 'break' is not inside a loop" "$GOT"
rm -f /tmp/_bc_err.knot

echo "== higher-order functions in --exec =="

# rk4 has a 2-arg callback (t, y) -> dy/dt -- previously interpreter-only.
cat > /tmp/_rk4.knot <<'EOF'
def dydt(t, y) { return y }
r = rk4(dydt, 0.0, 1.0, 1.0, 100)
print(r)
EOF
GOT_INTERP=$(./knot /tmp/_rk4.knot 2>&1)
GOT_EXEC=$(./knot --exec /tmp/_rk4.knot 2>/dev/null)
check "rk4 (interp) ~= e" "2.71828" "$GOT_INTERP"
check "rk4 (--exec) matches interp" "$GOT_INTERP" "$GOT_EXEC"
rm -f /tmp/_rk4.knot /tmp/knot__rk4.*

# 3-arg numerical callback (FnDDD_D path).
cat > /tmp/_h3.knot <<'EOF'
def apply3(g, a, b, c) { return g(a, b, c) }
def vol(l, w, h) { return l * w * h }
print(apply3(vol, 2.0, 3.0, 4.0))
EOF
GOT=$(./knot --exec /tmp/_h3.knot 2>/dev/null)
check "3-arg fn-ptr (--exec)" "24" "$GOT"
rm -f /tmp/_h3.knot /tmp/knot__h3.*

echo "== format =="

cat > /tmp/_fmt.knot <<'EOF'
print(format("x = %.3f", 3.14159265))
print(format("%d + %d = %d", 2, 3, 5))
print(format("|%-6s|%6s|", "ab", "cd"))
print(format("%05d", 7))
print(format("100%% done"))
EOF
GOT=$(./knot /tmp/_fmt.knot 2>&1 | tr '\n' '|')
check "format (interp)" "x = 3.142|2 + 3 = 5||ab    |    cd||00007|100% done|" "$GOT"
rm -f /tmp/_fmt.knot

# Type mismatch errors cleanly rather than crashing.
GOT=$(echo 'print(format("%d", "oops"))' | ./knot /dev/stdin 2>&1 | head -1)
check "format type mismatch errors" "error: format: %d expects num, got str" "$GOT"

echo "== lists =="

# append on a freshly-empty list, with mixed types.
cat > /tmp/_append_test.knot <<'EOF'
xs = []
append(xs, 1)
append(xs, "two")
append(xs, [10, 20])
print(len(xs), xs[0], xs[1], xs[-1])
EOF
GOT=$(./knot /tmp/_append_test.knot 2>&1)
check "append (interp)" "3 1 two [10, 20]" "$GOT"
rm -f /tmp/_append_test.knot

echo "== show statement =="

# The show statement labels each arg with its verbatim source text.
cat > /tmp/_show_test.knot <<'EOF'
x = 5
y = 10
show x
show x + y
show x, y, x * y
EOF
GOT=$(./knot /tmp/_show_test.knot 2>&1 | tr '\n' '|')
check "show (interp)" "x: 5|x + y: 15|x: 5|y: 10|x * y: 50|" "$GOT"
GOT=$(./knot --exec /tmp/_show_test.knot 2>/dev/null | tr '\n' '|')
check "show (--exec)" "x: 5|x + y: 15|x: 5|y: 10|x * y: 50|" "$GOT"
rm -f /tmp/_show_test.knot /tmp/knot__show_test.*

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" = "0" ]
