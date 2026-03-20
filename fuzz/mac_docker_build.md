# Mac Docker Build Guide

Step-by-step guide for building and running the WASM fuzzer on macOS (Apple Silicon) via Docker.

## Prerequisites

- **Docker Desktop** installed and running
- The rippled repo cloned locally
- No other tools needed on the Mac host — clang, conan, AFL++, Rust all live inside Docker

## Quick Start

```bash
# From the repo root (e.g. ~/CLionProjects/rippled)

# 1. Build the Docker image (one-time, ~10 min)
docker build -t rippled-fuzz fuzz/

# 2. Create a named volume for Conan cache (one-time)
docker volume create rippled-fuzz-conan

# 3. Start the container
docker run --rm -it --privileged \
  -v "$(pwd):/src" \
  -v rippled-fuzz-conan:/root/.conan2 \
  rippled-fuzz

# --- Everything below runs INSIDE the container ---

# 4. Build the fuzzer (~20 min first time, fast after)
cd /src/fuzz && ./build.sh afl

# 5. Create seed input and run
mkdir -p /src/fuzz/in
echo "111111111111111111111111111111111111111111111" > /src/fuzz/in/seed
AFL_MAP_SIZE=306084 afl-fuzz -i /src/fuzz/in -o /src/fuzz/out -t 3000 \
  -- /src/build-fuzz-afl/fuzz/wasm/wasm_fuzzer
```

> **Note:** `AFL_MAP_SIZE` may differ between architectures. If AFL rejects the
> value, the error message will tell you the correct one. Use 306084 for arm64,
> 301151 for x86_64.

## Resuming After a Crash

The fuzzer crashes after ~5 minutes due to `fundEnv()` state accumulation (known issue). Resume with:

```bash
AFL_MAP_SIZE=306084 afl-fuzz -i- -o /src/fuzz/out -t 3000 \
  -- /src/build-fuzz-afl/fuzz/wasm/wasm_fuzzer
```

## Coverage Build

The coverage build uses a standalone replay driver (not libFuzzer) to avoid
C++ stdlib ABI conflicts on arm64.

```bash
# Build coverage variant
cd /src/fuzz && ./build.sh coverage

# Replay the AFL corpus
cd /src
LLVM_PROFILE_FILE=fuzz.profraw \
  ./build-fuzz-coverage/fuzz/wasm/wasm_fuzzer /src/fuzz/out/default/queue/*

# Generate reports
llvm-profdata-21 merge -sparse fuzz.profraw -o fuzz.profdata

# Summary
llvm-cov-21 report ./build-fuzz-coverage/fuzz/wasm/wasm_fuzzer \
  -instr-profile=fuzz.profdata

# HTML report (viewable on Mac host at fuzz/coverage-report/index.html)
llvm-cov-21 show ./build-fuzz-coverage/fuzz/wasm/wasm_fuzzer \
  -instr-profile=fuzz.profdata \
  -format=html -output-dir=/src/fuzz/coverage-report
```

## ARM64 (Apple Silicon) Technical Details

The Docker image auto-detects the host architecture. On Apple Silicon (M1–M4),
it builds natively for arm64 with the following adaptations:

| Issue | Cause | Solution |
|-------|-------|----------|
| `cstddef` not found when building boost | clang-21 can't find GCC's libstdc++ headers on arm64 | Use clang's own libc++ instead of libstdc++ |
| `-static-libstdc++` linker error | rippled CMake adds this flag when `static=True` | Pass `-o static=False` to conan on arm64 |
| boost locale/stacktrace build failure | These components don't build with libc++ | Disabled via conan options (fuzzer doesn't need them) |
| libFuzzer ABI mismatch (coverage build) | `libclang_rt.fuzzer` is pre-built against libstdc++ | Coverage uses standalone replay driver instead of libFuzzer |
| `-DCMAKE_CXX_FLAGS` overrides conan toolchain | Passing cmake flags drops `-stdlib=libc++` | `STDLIB_FLAG` variable explicitly includes it |

On x86_64 Linux, none of these adaptations activate — the build uses
libstdc++11 as before.

## Other Build Variants

```bash
# AddressSanitizer
./build.sh asan

# UndefinedBehaviorSanitizer
./build.sh ubsan

# Host function fuzzer (FUZZ_HOST mode)
./build.sh host afl
```

## Cleaning Up

```bash
# Remove build artifacts (inside container)
rm -rf /src/build-fuzz-afl /src/build-fuzz-coverage

# Remove conan cache (from Mac host, if you need a fresh start)
docker volume rm rippled-fuzz-conan

# Remove Docker image
docker rmi rippled-fuzz
```

## Known Issues

- **Fuzzer crashes after ~5 min** — `fundEnv()` accumulates ledger state in
  persistent mode. Workaround: resume with `-i-`. Fix: make `fundEnv()` idempotent.
- **Stability ~36%** — background threads in `ApplicationImp` cause
  non-determinism. Accepted limitation of the current architecture.
- **Nearly empty ledger** — only 3 accounts and 1 escrow. ~85% of host function
  calls fail with NOT_FOUND. Enriching ledger state is a planned improvement.
