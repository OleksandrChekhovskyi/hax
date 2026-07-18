# Build/test/lint entry points for humans, editors (nvim :make), and coding
# agents. Delegates to scripts/check.sh, which keeps successful output compact
# and lets failures pass through untouched.

BUILD_DIR ?= build

.PHONY: all tests lint clean

all:
	@BUILD_DIR=$(BUILD_DIR) scripts/check.sh build

tests:
	@BUILD_DIR=$(BUILD_DIR) scripts/check.sh test

lint:
	@scripts/check.sh lint

clean:
	rm -rf $(BUILD_DIR)
