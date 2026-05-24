CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
SRC      := src/main.cpp
HDRS     := $(wildcard src/*.hpp)
TARGET   := knot
STDLIB   := stdlib/stdlib.knot
EMBED    := src/stdlib_embedded.hpp

# WASM build via emscripten. Compiles the tree-walking interpreter
# (no --exec, no shelling out to cc) and exports knot_run / knot_free
# as a single-file module the browser playground can load directly.
EMCC      ?= emcc
WASM_SRC  := src/wasm_entry.cpp
WASM_OUT  := web/knot.js

$(TARGET): $(SRC) $(HDRS) $(EMBED)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

# Generate a C++ header containing the stdlib source as a string literal.
# Using a raw-string with a randomly-chosen delimiter so the stdlib text
# doesn't need any escaping (and can contain quotes, backslashes, etc).
$(EMBED): $(STDLIB) scripts/embed_stdlib.sh
	@mkdir -p src
	./scripts/embed_stdlib.sh $(STDLIB) > $(EMBED)

# Run the smoke test suite (interpreter+transpiler parity on examples,
# tag-detection sanity) followed by the in-language unit tests under
# tests/. A passing `make test` means both layers are green.
test: $(TARGET)
	./scripts/smoke_test.sh
	./scripts/run_unit_tests.sh

# Browser playground. Run `make wasm` (requires `emcc` on PATH; install
# with `brew install emscripten`) then serve the web/ dir over HTTP --
# e.g. `python3 -m http.server 8000 --directory web` -- and open
# http://localhost:8000.
#
# Produces TWO files: web/knot.js (small loader) and web/knot.wasm
# (the actual module). The browser fetches them in parallel and
# compiles the WASM via its native streaming path -- considerably
# faster than the SINGLE_FILE embed-as-base64 approach.
#
# Flags:
#   -O2                       optimization parity with the native build
#   -fexceptions              C++ throw/catch -- the interpreter relies
#                             on ReturnSignal/BreakSignal/Diag throws
#   -sMODULARIZE=1            export Knot() factory rather than dirtying
#                             the global scope on load
#   -sEXPORT_NAME='Knot'      name of the factory the playground awaits
#   -sNO_EXIT_RUNTIME=1       keep the runtime alive between calls
#   -sALLOW_MEMORY_GROWTH=1   long traces / matrices don't OOM at the
#                             default 16MB heap
#   -sEXPORTED_FUNCTIONS       just the two we wrote + malloc/free
#   -sEXPORTED_RUNTIME_METHODS  ccall + UTF8ToString for JS <-> C str
wasm: $(WASM_SRC) $(HDRS) $(EMBED)
	@mkdir -p web
	$(EMCC) $(CXXFLAGS) -fexceptions -o $(WASM_OUT) $(WASM_SRC) \
	  -fexceptions \
	  -sMODULARIZE=1 \
	  -sEXPORT_NAME='Knot' \
	  -sNO_EXIT_RUNTIME=1 \
	  -sALLOW_MEMORY_GROWTH=1 \
	  -sEXPORTED_FUNCTIONS="['_knot_run','_knot_free','_malloc','_free']" \
	  -sEXPORTED_RUNTIME_METHODS="['ccall','UTF8ToString']"

clean:
	rm -f $(TARGET) $(EMBED) $(WASM_OUT)

.PHONY: clean test wasm

