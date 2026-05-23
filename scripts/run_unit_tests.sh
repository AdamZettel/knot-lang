#!/bin/bash
# Run every tests/*.knot file through `./knot --test`. Each file is
# expected to contain one or more `test "name" { ... }` blocks and to
# end its output with a summary line of the form:
#   <filename>: N passed, M failed
#
# This script aggregates those summaries, prints failures with the
# offending file's name, and exits 0 only if every test passed.
#
# Run from the knot repo root:
#   ./scripts/run_unit_tests.sh

set -e
cd "$(dirname "$0")/.."

if [ ! -x ./knot ]; then
    echo "no ./knot binary -- run \`make\` first"
    exit 1
fi

if [ ! -d tests ]; then
    echo "no tests/ directory -- nothing to run"
    exit 0
fi

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_FILES=0
FAILED_FILES=()

# Iterate test files in stable (sorted) order. `shopt -s nullglob` would
# be the bash-only safe form; fall back to a guarded find for portability.
TEST_FILES=$(find tests -maxdepth 1 -name '*.knot' | sort)

if [ -z "$TEST_FILES" ]; then
    echo "no tests/*.knot files -- nothing to run"
    exit 0
fi

for f in $TEST_FILES; do
    TOTAL_FILES=$((TOTAL_FILES + 1))
    OUT=$(./knot --test "$f" 2>&1 || true)

    # The summary line is the last line of output, of the form:
    #   <filename>: N passed, M failed
    SUMMARY=$(echo "$OUT" | tail -1)

    # Parse N and M out of the summary. If the line doesn't match the
    # expected shape, treat the whole file as a failure.
    P=$(echo "$SUMMARY" | sed -nE 's/^.*: ([0-9]+) passed, [0-9]+ failed$/\1/p')
    FAIL=$(echo "$SUMMARY" | sed -nE 's/^.*: [0-9]+ passed, ([0-9]+) failed$/\1/p')
    if [ -z "$P" ] || [ -z "$FAIL" ]; then
        echo "$f: did not produce a parseable summary line, output:"
        echo "$OUT" | sed 's/^/  > /'
        FAILED_FILES+=("$f")
        continue
    fi

    TOTAL_PASS=$((TOTAL_PASS + P))
    TOTAL_FAIL=$((TOTAL_FAIL + FAIL))

    if [ "$FAIL" -eq 0 ]; then
        echo "$f: $P passed"
    else
        echo "$f: $P passed, $FAIL FAILED"
        # Print the FAIL lines (and any indented continuation lines for
        # their messages) for surgical context.
        echo "$OUT" | awk '
            /^  FAIL/   { in_fail = 1; print; next }
            /^        / { if (in_fail) print; next }
            /^  PASS/   { in_fail = 0; next }
            { in_fail = 0 }
        '
        FAILED_FILES+=("$f")
    fi
done

echo ""
echo "unit-test totals: $TOTAL_PASS passed, $TOTAL_FAIL failed across $TOTAL_FILES files"

if [ "$TOTAL_FAIL" -ne 0 ] || [ "${#FAILED_FILES[@]}" -ne 0 ]; then
    echo "failed files:"
    for f in "${FAILED_FILES[@]}"; do echo "  $f"; done
    exit 1
fi
exit 0
