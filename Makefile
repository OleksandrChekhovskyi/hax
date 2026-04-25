# Convenience Makefile for nvim :make integration.

BUILD_DIR := build

.PHONY: all tests lint clean

all:
	@[ -d $(BUILD_DIR) ] || meson setup $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

tests: all
	meson test -C $(BUILD_DIR) --print-errorlogs

lint:
	@find src tests -type f \( -name '*.c' -o -name '*.h' \) -print0 \
		| xargs -0 clang-format --dry-run --Werror

clean:
	rm -rf $(BUILD_DIR)
