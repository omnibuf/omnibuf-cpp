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

# Run the language-independent OmniBuf conformance suite across all 4 tiers
conformance adapter="./build/conformance-cpp": build
    /Users/kevin/Projects/omnibuf/omnibuf-go/bin/conformance-runner "{{adapter}}"

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
