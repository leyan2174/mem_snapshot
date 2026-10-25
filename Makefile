CC ?= cc
CFLAGS ?= -O2 -g
WARN = -Wall -Wextra -Werror
COMMON = -std=c11 $(WARN) -fno-omit-frame-pointer -fno-optimize-sibling-calls

.PHONY: all test benchmark clean
all: build/libmem_snapshot.so build/fixture build/fixture-nopie build/bench build/demo

build:
	mkdir -p build

build/libmem_snapshot.so: mem_snapshot.c | build
	$(CC) $(CFLAGS) $(COMMON) -fPIC -shared -Wl,-z,defs -Wl,-z,now -o $@ $< -ldl -pthread

build/fixture: tests/fixture.c | build
	$(CC) $(CFLAGS) $(COMMON) -fno-builtin -rdynamic -o $@ $< -pthread -ldl

build/fixture-nopie: tests/fixture.c | build
	$(CC) $(CFLAGS) $(COMMON) -fno-builtin -no-pie -rdynamic -o $@ $< -pthread -ldl

build/bench: tests/bench.c | build
	$(CC) $(CFLAGS) $(COMMON) -fno-builtin -o $@ $< -pthread

build/demo: examples/demo.c | build
	$(CC) $(CFLAGS) $(COMMON) -fno-builtin -rdynamic -o $@ $<

test: all
	python3 tests/test_snapshot.py

benchmark: all
	python3 tests/benchmark.py

clean:
	rm -f build/libmem_snapshot.so build/fixture build/fixture-nopie build/bench build/demo
