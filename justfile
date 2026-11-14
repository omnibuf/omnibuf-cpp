# List available recipes
default:
    @just --list

# Configure CMake Release build directory
configure:
    cmake -B build -DCMAKE_BUILD_TYPE=Release

# Lint schemas, generate C++20 OmniBuf headers via buf.gen.yaml, and build FileDescriptorSet image
proto: configure
    cmake --build build --target protoc-gen-omnibuf-cpp -j
    @mkdir -p build/gen
    buf lint
    buf generate
    buf build -o build/gen/schemas.fds.pb --exclude-source-info

# Alias for proto code generation
generate: proto

# Build all C++20 targets (plugin, conformance adapter, unit tests, benchmark suite)
build: configure
    cmake --build build -j

# Run CTest unit test suite
test: build
    ctest --test-dir build --output-on-failure

# Run the language-independent 758-case OmniBuf conformance suite across all 4 tiers
conformance adapter="./build/conformance-cpp": build
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -n "${CONFORMANCE_RUNNER:-}" ]]; then
        RUNNER="$CONFORMANCE_RUNNER"
    elif [[ -x "../omnibuf-go/bin/conformance-runner" ]]; then
        RUNNER="../omnibuf-go/bin/conformance-runner"
    elif command -v conformance-runner >/dev/null 2>&1; then
        RUNNER="conformance-runner"
    elif command -v go >/dev/null 2>&1; then
        exec go run github.com/omnibuf/omnibuf-go/cmd/conformance-runner@71dad83aadd7dc65401d662521d34cfaeb1011ab "{{adapter}}"
    else
        echo "conformance-runner binary not found; set CONFORMANCE_RUNNER or install conformance-runner" >&2
        exit 1
    fi
    "$RUNNER" "{{adapter}}"

# Run reproducible PeerUpdate benchmarks (-O3 -DNDEBUG) and emit testdata/benchmarks.json
bench: build
    ./build/omnibuf_bench

# Check Protobuf formatting/lint rules and strict C++20 compiler warnings (-Wall -Wextra -Wpedantic -Werror)
lint:
    buf lint
    buf format -d --exit-code
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --clean-first -j

# Full continuous integration gate
ci: lint proto test conformance
