CC = clang
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=12.0
LDLIBS = -framework CoreFoundation -framework IOKit
BIN = dist/dont-touch-monitor
SOURCES = src/main.c src/history.c

.PHONY: all universal test clean
all: $(BIN)

$(BIN): $(SOURCES) src/history.h Makefile
	mkdir -p dist
	$(CC) $(CFLAGS) $(SOURCES) $(LDLIBS) -o $@

universal: $(SOURCES) src/history.h Makefile
	mkdir -p dist
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 $(SOURCES) $(LDLIBS) -o $(BIN)
	codesign --force --sign - $(BIN)

test: all
	$(CC) $(CFLAGS) tests/history_test.c src/history.c -o dist/history-test
	./dist/history-test
	python3 tests/integration.py $(BIN)

clean:
	rm -rf dist
