CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
SRC      := src/main.cpp
HDRS     := $(wildcard src/*.hpp)
TARGET   := knot
STDLIB   := stdlib/stdlib.knot
EMBED    := src/stdlib_embedded.hpp

$(TARGET): $(SRC) $(HDRS) $(EMBED)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

# Generate a C++ header containing the stdlib source as a string literal.
# Using a raw-string with a randomly-chosen delimiter so the stdlib text
# doesn't need any escaping (and can contain quotes, backslashes, etc).
$(EMBED): $(STDLIB) scripts/embed_stdlib.sh
	@mkdir -p src
	./scripts/embed_stdlib.sh $(STDLIB) > $(EMBED)

# Run the smoke test suite. Exercises both interpreter and transpiler on
# the example programs, and verifies the axis-swap detection still fires.
test: $(TARGET)
	./scripts/smoke_test.sh

clean:
	rm -f $(TARGET) $(EMBED)

.PHONY: clean test

