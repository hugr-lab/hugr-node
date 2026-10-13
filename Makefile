PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hugr_node
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# duckdb-acl's Flight door is part of the built-in acl: Arrow comes from vcpkg (duckdb-acl's Makefile says why),
# ACL_NO_FLIGHT=1 drops it for a fast local loop
ifeq ($(ACL_NO_FLIGHT),)
USE_MERGED_VCPKG_MANIFEST := 1
VCPKG_TOOLCHAIN_PATH ?= $(PROJ_DIR)vcpkg/scripts/buildsystems/vcpkg.cmake
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- the platform part's C++ test (duckdb-acl's style: one program per test/cpp/test_*.cpp, linked against the
# release libduckdb, which carries hugr_node statically) --------------------------------------------------------
TEST_CPP_SOURCES := $(wildcard test/cpp/test_*.cpp)
TEST_CPP_DIR := build/test
TEST_CPP_BINS := $(patsubst test/cpp/%.cpp,$(TEST_CPP_DIR)/%,$(TEST_CPP_SOURCES))
TEST_CPP_INCLUDES := -I duckdb/src/include -I duckdb/third_party/fmt/include -I src/include -I duckdb-acl/src/include \
	-I duckdb-acl/test/cpp -I duckdb-acl/duckdb-ext-common/contracts
ifeq ($(shell uname),Darwin)
TEST_CPP_DUCKDB_LIB := build/release/src/libduckdb.dylib
else
TEST_CPP_DUCKDB_LIB := build/release/src/libduckdb.so
endif
TEST_CPP_LINK = -L build/release/src -lduckdb -Wl,-rpath,$(abspath build/release/src)

$(TEST_CPP_DIR)/%: test/cpp/%.cpp
	@mkdir -p $(TEST_CPP_DIR)
	$(CXX) -std=c++17 -O2 -DNDEBUG -pthread $(TEST_CPP_INCLUDES) $< -o $@ $(TEST_CPP_LINK)

.PHONY: test-cpp
test-cpp:
	@test -f $(TEST_CPP_DUCKDB_LIB) || { echo "test-cpp: $(TEST_CPP_DUCKDB_LIB) missing - run 'GEN=ninja make' first" >&2; exit 1; }
	@$(MAKE) --no-print-directory $(TEST_CPP_BINS)
	@for t in $(TEST_CPP_BINS); do echo "== $$t"; $$t || exit 1; done
