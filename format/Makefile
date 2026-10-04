CC := clang
CXX := clang++
CFLAGS := -Wall -Wextra -Werror -pedantic -Iinclude

check: build/test_format_c build/test_format_cpp
	./build/test_format_c
	./build/test_format_cpp
	$(CC) -E -P -x assembler-with-cpp -D__ASSEMBLER__ -Iinclude tests/test_format.c -o build/test_format.s

build/test_format_c: tests/test_format.c $(wildcard include/aicaflow/*.h)
	@mkdir -p build
	$(CC) -std=c11 $(CFLAGS) $< -o $@

build/test_format_cpp: tests/test_format.c $(wildcard include/aicaflow/*.h)
	@mkdir -p build
	$(CXX) -x c++ -std=c++11 $(CFLAGS) $< -o $@

.PHONY: check
