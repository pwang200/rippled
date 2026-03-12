# Fuzzer Build and Run — macOS Notes (WIP)

This document captures the steps, observations, and limitations discovered while
getting the WASM fuzzer running on macOS. The intended audience is a developer
on a Mac who wants to run the fuzzer.

---

## 1. Running the Fuzzer via Docker

Docker provides a Linux environment on macOS and is the only supported way to
run AFL++ on Mac (see Section 3 for why native does not work).

### Prerequisites

- Docker Desktop installed and running
- The rippled repo cloned locally

### Step 1 — Build the Docker image (one-time, ~10 min)

Run from the repo root:

```bash
docker build -t rippled-fuzz fuzz/
```

This builds a Ubuntu 24.04 image with clang-21, AFL++ (compiled from source
against LLVM 21), Rust, and Conan 2 pre-configured.

### Step 2 — Create a named volume for the Conan cache (one-time)

```bash
docker volume create rippled-fuzz-conan
```

This volume persists downloaded Conan packages across container restarts.
Without it, `conan install` re-downloads everything on every run (~20 min).

### Step 3 — Start the container

```bash
docker run --rm -it --privileged \
  -v "$(pwd):/src" \
  -v rippled-fuzz-conan:/root/.conan2 \
  rippled-fuzz
```

- `--privileged` is required so AFL can configure `/proc/sys/kernel/core_pattern`
- `-v "$(pwd):/src"` mounts the repo — edits on Mac are instantly visible inside
- `-v rippled-fuzz-conan:/root/.conan2` mounts the persistent Conan cache

### Step 4 — Build the fuzzer (first time: ~20 min; subsequent: fast)

Inside the container:

```bash
cd /src/fuzz && ./build.sh afl
```

Output binary: `/src/build-fuzz-afl/fuzz/wasm/wasm_fuzzer`

For the host function fuzzer:

```bash
cd /src/fuzz && ./build.sh host afl
cd /src/fuzz/autarkie-fuzzer && cargo build --release
```

### Step 5 — Run the AFL++ general WASM fuzzer

```bash
AFL_MAP_SIZE=301151 afl-fuzz -i /src/fuzz/in -o /src/fuzz/out -t 3000 \
  -- /src/build-fuzz-afl/fuzz/wasm/wasm_fuzzer
```

Key flags:
- `AFL_MAP_SIZE=301151` — required; the binary has 301,151 coverage edges which
  exceeds AFL's default map size
- `-t 3000` — 3-second timeout per input; must appear **before** `--`, not after
- `-i-` instead of `-i /src/fuzz/in` on subsequent runs to resume from existing corpus

### Step 6 — Restart after crash (expected every ~12 min)

The fuzzer crashes after ~12 minutes due to ledger state accumulation (see
Section 2). Resume without losing progress:

```bash
AFL_MAP_SIZE=301151 afl-fuzz -i- -o /src/fuzz/out -t 3000 \
  -- /src/build-fuzz-afl/fuzz/wasm/wasm_fuzzer
```

### Step 7 — Run the autarkie grammar-based host function fuzzer

```bash
cd /src/fuzz/autarkie-fuzzer
./target/release/autarkie-fuzzer -c 0-$(nproc) -o ./out -t 3 -r \
  /src/build-fuzz-host-afl/fuzz/wasm/wasm_fuzzer
```

Key differences from AFL:
- `-t` is in **seconds** (not milliseconds)
- Automatically uses all available cores via `-c 0-$(nproc)`
- No `AFL_MAP_SIZE` needed

---

## 2. Docker Build — Observations, Limitations, Recommendations

### Observations

- **AFL++ must be compiled from source against LLVM 21.** The Ubuntu apt package
  ships AFL++ compiled against LLVM 17. Its LLVM pass plugins
  (`SanitizerCoveragePCGUARD.so`, `cmplog-*.so`) cannot be loaded by clang-21
  because the LLVM API changed between versions 17 and 21. The Dockerfile builds
  AFL++ from source with `LLVM_CONFIG=llvm-config-21`.

- **Conan profile must be explicitly set to clang-21.** `conan profile detect`
  auto-detects GCC (from `build-essential`) and ignores `CC`/`CXX` env vars.
  The Dockerfile explicitly writes the default profile after detection.

- **`AFL_MAP_SIZE=301151` is required.** The instrumented binary has 301,151
  coverage edges. Without this, AFL rejects the binary with a fork server crash.

- **`-t 3000` must be an afl-fuzz flag, not a target argument.** Placing it
  after `--` passes it to the libFuzzer driver, which interprets it as a corpus
  path, causes N=0 assertion, and crashes the fuzzer.

- **exec/sec is ~44 (slow).** The bottleneck is `jtx::Env` initialization —
  a full rippled application instance (RocksDB, job queue, network threads) is
  created once per fork and cannot be avoided. This is inherent to the fuzzer
  architecture which requires a real ledger for host function execution.

- **Stability is ~36%.** Background threads in `ApplicationImp` cause
  non-deterministic coverage maps for the same input. AFL's guided mutation is
  weakened but crash/hang detection is unaffected.

### Limitations

- **AFL fuzzer crashes after ~12 minutes** with "Unable to communicate with fork
  server". Root cause: persistent mode runs many inputs per forked child;
  `fundEnv()` is called on every iteration adding accounts and escrows
  accumulating without cleanup; the child eventually dies from state corruption
  or resource exhaustion. Workaround: restart with `-i-` to resume from corpus.

- **Autarkie fuzzer slows dramatically after ~40 minutes** (from ~33 exec/sec to
  ~1 exec/sec). Same root cause as the AFL crash — persistent children
  accumulating ledger state. No built-in loopcount mechanism in autarkie.
  Workaround: restart periodically.

- **`AFL_FUZZER_LOOPCOUNT` causes false positive crashes.** Setting it to a
  small value (e.g., 1 or 100) causes the persistent child to exit cleanly after
  N inputs, but AFL interprets the exit as a crash (SIGSEGV). The destructor of
  `jtx::Env` also crashes on teardown, compounding the false positives.

### Recommendations

- **Fix `fundEnv()` idempotency.** Currently it unconditionally funds accounts
  and creates escrows on every call without checking if they already exist. This
  is the root cause of state accumulation and the ~12-min crash. Making it
  idempotent (check before fund) would eliminate the crash and potentially
  improve stability.

- **Run on a dedicated Linux machine for production campaigns.** Docker on a
  MacBook provides correct results but 12 CPU cores at 44 exec/sec is limiting.
  A 32-64 core bare metal Linux machine would provide proportionally more
  coverage per hour.

- **Automate fuzzer restart.** Since the AFL fuzzer crashes predictably every
  ~12 min, a simple shell loop can keep it running unattended:
  ```bash
  while true; do
    AFL_MAP_SIZE=301151 afl-fuzz -i- -o /src/fuzz/out -t 3000 \
      -- /src/build-fuzz-afl/fuzz/wasm/wasm_fuzzer
    sleep 2
  done
  ```

---

## 3. macOS Native Build — Observations, Limitations, Recommendations

### Observations

- **`build.sh` works on macOS after OS-aware fixes.** The script was updated to
  detect the OS via `uname -s` and set platform-specific variables:
  - `LIBCXX=libc++` (macOS) vs `libstdc++11` (Linux)
  - `LINKER_FLAGS=()` on macOS (no lld; macOS uses its own ld64 linker)
  - `AFL_ENV=(AFL_CC=clang-21 AFL_CXX=clang++)` pointing to llvm@21 from
    Homebrew at `/usr/local/opt/llvm@21/bin/`
  - `NPROC=$(sysctl -n hw.ncpu)` instead of `$(nproc)`

- **The fuzzer binary compiles successfully on macOS.** `build.sh afl` completes
  and produces `wasm_fuzzer` with exit code 0.

### Limitations

- **AFL++ cannot run on macOS with this target.** This is a fundamental macOS
  limitation, not a fixable bug. AFL++ uses a fork server: it forks the target
  process and feeds inputs to the child. On macOS, `fork()` in a multithreaded
  process is unsafe — the child only inherits the calling thread, but other
  threads' locks and state are not inherited, leading to deadlocks or crashes.
  `jtx::Env` creates `ApplicationImp` which spawns multiple background threads
  during `LLVMFuzzerInitialize`, before the fork server can initialize. The
  result is a consistent `SIGABRT` in the forked child.

- **This is not solvable without changing the fuzzer architecture.** The threads
  are created by rippled's application layer which cannot be easily made
  single-threaded for fuzzing purposes.

### Recommendations

- **Use Docker for all fuzzing on macOS.** Docker runs a Linux VM underneath,
  which has no fork+multithread limitation. There are no meaningful performance
  or correctness differences vs bare metal Linux for this use case.

- **Keep macOS build support.** The `build.sh` macOS support is useful for
  compilation checks and iterating on the fuzzer harness code without needing
  Docker. A developer can build on Mac to catch compilation errors, then run the
  actual fuzzing in Docker.

- **Do not run `build.sh` on macOS before fuzzing in Docker.** The
  `build-fuzz-afl/` directory created by a Mac build has absolute paths baked
  into `CMakeCache.txt`. Docker mounts the repo at `/src` (different path),
  causing CMake to reject the cache. Always delete Mac-built directories before
  building in Docker:
  ```bash
  rm -rf build-fuzz-afl build-fuzz-asan build-fuzz-ubsan build-fuzz-coverage
  ```
