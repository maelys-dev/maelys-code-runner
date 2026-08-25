.PHONY: all check asan-ubsan tsan fuzz-smoke install install-check package-release clean

BUILD_ROOT ?= build
BUILD_DIR ?= $(BUILD_ROOT)/release
CMAKE ?= cmake
PYTHON ?= python3
TARGET ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
FUZZ_CC ?= $(shell if test -x /opt/homebrew/opt/llvm/bin/clang; then \
	printf '%s' /opt/homebrew/opt/llvm/bin/clang; else command -v clang; fi)

all:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) --parallel

check: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure

asan-ubsan:
	$(CMAKE) -S . -B $(BUILD_ROOT)/asan-ubsan -DCMAKE_BUILD_TYPE=Debug \
		-DMAELYS_CODE_RUNNER_ENABLE_ASAN=ON \
		-DMAELYS_CODE_RUNNER_ENABLE_UBSAN=ON
	$(CMAKE) --build $(BUILD_ROOT)/asan-ubsan --parallel
	ctest --test-dir $(BUILD_ROOT)/asan-ubsan --output-on-failure

tsan:
	$(CMAKE) -S . -B $(BUILD_ROOT)/tsan -DCMAKE_BUILD_TYPE=Debug \
		-DMAELYS_CODE_RUNNER_ENABLE_TSAN=ON
	$(CMAKE) --build $(BUILD_ROOT)/tsan --parallel
	ctest --test-dir $(BUILD_ROOT)/tsan --output-on-failure

fuzz-smoke:
	$(CMAKE) -S . -B $(BUILD_ROOT)/fuzz -DCMAKE_BUILD_TYPE=Debug \
		-DMAELYS_CODE_RUNNER_BUILD_FUZZER=ON \
		-DCMAKE_C_COMPILER=$(FUZZ_CC)
	$(CMAKE) --build $(BUILD_ROOT)/fuzz --target fuzz-code-bridge --parallel
	$(BUILD_ROOT)/fuzz/fuzz-code-bridge -runs=2000 -max_len=8192

install: all
	$(CMAKE) --install $(BUILD_DIR)

install-check: all
	$(CMAKE) --install $(BUILD_DIR) --prefix $(BUILD_ROOT)/install-root
	@test "$$($(BUILD_ROOT)/install-root/bin/maelys-code-runner --version)" = "$$(cat VERSION)"
	@test -f $(BUILD_ROOT)/install-root/share/doc/maelys-code-runner/licenses/LICENSE.quickjs-ng
	@test -f $(BUILD_ROOT)/install-root/share/doc/maelys-code-runner/licenses/LICENSE.jansson

package-release:
	$(PYTHON) scripts/package-release.py $(TARGET)

clean:
	$(CMAKE) -E remove_directory $(BUILD_ROOT)
