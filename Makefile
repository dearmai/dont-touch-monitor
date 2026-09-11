CC = clang
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=12.0
LDLIBS = -framework CoreFoundation -framework IOKit
BIN = dist/dont-touch-monitor

.PHONY: all universal test clean
all: $(BIN)

$(BIN): src/main.c Makefile
	mkdir -p dist
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

universal: src/main.c Makefile
	mkdir -p dist
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 $< $(LDLIBS) -o $(BIN)
	codesign --force --sign - $(BIN)

test: all
	python3 tests/integration.py $(BIN)

clean:
	rm -rf dist
