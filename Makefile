PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hugr_node
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# vcpkg always: the built-in acl's Flight door needs Arrow (duckdb-acl's Makefile says why) and the built-in tresor
# OpenSSL (tresor spec 024) - the platform node is one configuration, the one it ships in. Bootstrap once with
# `make vcpkg-setup`, or point VCPKG_TOOLCHAIN_PATH at an existing vcpkg checkout (duckdb-acl's will do)
USE_MERGED_VCPKG_MANIFEST := 1
VCPKG_TOOLCHAIN_PATH ?= $(PROJ_DIR)vcpkg/scripts/buildsystems/vcpkg.cmake

# Include the Makefile from extension-ci-tools
# Spec 009: extension-ci-tools a90d6d4 (2026-10-15) hands the distribution build a prebuilt static duckdb whose
# version (the release's alpha) is not the one the loadable names (the branch): duckdb-acl (spec 133) and acl-otel
# (spec 026) found it. hugr_node builds duckdb from the pinned source, as they do.
override DUCKDB_PREBUILT_LIBRARY :=
override DUCKDB_PREBUILT_EXTENSIONS :=
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- the platform part's C++ test (duckdb-acl's style: one program per test/cpp/test_*.cpp, linked against the
# release libduckdb, which carries hugr_node statically) --------------------------------------------------------
TEST_CPP_SOURCES := $(wildcard test/cpp/test_*.cpp)
TEST_CPP_DIR := build/test
TEST_CPP_BINS := $(patsubst test/cpp/%.cpp,$(TEST_CPP_DIR)/%,$(TEST_CPP_SOURCES))
TEST_CPP_INCLUDES := -I duckdb/src/include -I duckdb/third_party/fmt/include -I src/include -I src/hugr_type/include -I duckdb-acl/src/include \
	-I duckdb-acl/test/cpp -I duckdb-acl/duckdb-ext-common/contracts
ifeq ($(shell uname),Darwin)
TEST_CPP_DUCKDB_LIB := build/release/src/libduckdb.dylib
else
TEST_CPP_DUCKDB_LIB := build/release/src/libduckdb.so
endif
TEST_CPP_LINK = -L build/release/src -lduckdb -Wl,-rpath,$(abspath build/release/src)

# the built library is a prerequisite: a binary compiled against another duckdb's headers aborts when it loads this
# one (found re-pinning to 7304f7b, spec 007), so a rebuilt duckdb rebuilds the tests
$(TEST_CPP_DIR)/%: test/cpp/%.cpp $(TEST_CPP_DUCKDB_LIB)
	@mkdir -p $(TEST_CPP_DIR)
	$(CXX) -std=c++17 -O2 -DNDEBUG -pthread $(TEST_CPP_INCLUDES) $< -o $@ $(TEST_CPP_LINK)

.PHONY: test-cpp
test-cpp:
	@test -f $(TEST_CPP_DUCKDB_LIB) || { echo "test-cpp: $(TEST_CPP_DUCKDB_LIB) missing - run 'GEN=ninja make' first" >&2; exit 1; }
	@$(MAKE) --no-print-directory $(TEST_CPP_BINS)
	@for t in $(TEST_CPP_BINS); do echo "== $$t"; $$t || exit 1; done

# --- end to end (spec 002): tresor inside the cluster - a real tresor-server (TRESOR_SERVER_DIR=<checkout>, else its
# main is cloned) on SQLite, Keycloak in docker, two hugr_node nodes. Needs the build, docker, go; SKIPs without them.
# spec 005: acl_otel's export from a node to a fake OTLP receiver (python) - inert with no endpoint, records after one.
# spec 006: a node bootstraps itself (Keycloak, tresor-server, Postgres, kista, a fake control plane; needs the
# HUGR_NODE_E2E=1 build for its postgres_scanner)
# spec 011: the hugr type across processes - two B nodes behind a stand-in front, A attached through it (python only);
# its identity through tresor - A's link a secret whose token tresor mints for the node (Keycloak, tresor-server)
.PHONY: test-e2e
test-e2e:
	test/e2e/tresor/run.sh
	test/e2e/otel/run.sh
	test/e2e/bootstrap/run.sh
	test/e2e/federation/run.sh
	test/e2e/federation/tresor.sh

# a local vcpkg checkout, as duckdb-acl's `make vcpkg-setup`
.PHONY: vcpkg-setup
vcpkg-setup:
	@test -d vcpkg || git clone https://github.com/microsoft/vcpkg.git vcpkg
	./vcpkg/bootstrap-vcpkg.sh -disableMetrics
