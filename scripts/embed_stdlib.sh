#!/bin/bash
# Convert stdlib/stdlib.knot into a C++ header that exposes the stdlib source
# as a string constant. The raw-string delimiter "KNOT_STDLIB" is
# vanishingly unlikely to appear in the stdlib text.
set -e
STDLIB="$1"
if [ -z "$STDLIB" ]; then
    echo "usage: $0 path/to/stdlib.knot" >&2
    exit 1
fi
cat <<EOF
#pragma once
// Auto-generated from $STDLIB. Do not edit by hand; edit the source instead.
namespace knot {
inline const char* kStdlibSource = R"KNOT_STDLIB(
$(cat "$STDLIB")
)KNOT_STDLIB";
} // namespace knot
EOF
