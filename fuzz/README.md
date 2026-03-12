# Fuzzing

This directory contains two fuzzers. One general wasm fuzzer, including host functions and one fuzzer that focuses solely on host functions.
Both are instrumented by [AFL++](https://github.com/AFLplusplus/AFLplusplus/).

# Maintenance
The fuzzers require maintenance in two different scenarios:

## Changes in host function signatures
Whenever a new host function is added, or an existing one's signature (input or output parameters) changes, the configurations for the fuzzers must be updated.
Look at ``available_imports`` in ``wasm-smith-lib/src/lib.rs``. Here, you can see how a function is defined. For example:
```
(import "env" "get_tx_field" (func $get_tx_field (param i32 i32 i32) (result i32)))
```

## Changes in wasm features
At the time of writing, there are quite some constraints on what wasm features and instructions are enabled. Thus, we limit our fuzzer's instruction set to make sure
our generation is more accurate.
If this changes, you must update the ``allowed_instructions`` field in ``wasm-smith-lib/src/lib.rs`` to include other instruction kinds.
See the available ``IntructionKind`` at ``wasm-smith``'s [documentation](https://docs.rs/wasm-smith/latest/wasm_smith/enum.InstructionKind.html).

# Docker (recommended on macOS)

AFL++ requires Linux. On macOS the fork-server crashes because `jtx::Env`
spawns threads before AFL's fork point. Use the provided `Dockerfile` to get
a ready-made Linux environment.

```bash
# 1. Build the image once (from the repo root):
docker build -t rippled-fuzz fuzz/

# 2. Create a named volume for the conan cache so packages survive restarts:
docker volume create rippled-fuzz-conan

# 3. Start an interactive container with the repo mounted at /src:
docker run --rm -it \
  --privileged \
  -v "$(pwd):/src" \
  -v rippled-fuzz-conan:/root/.conan2 \
  rippled-fuzz

# 4. Inside the container — build and fuzz:
cd /src
./fuzz/build.sh afl
echo "111111111111111111111111111111111111111111111" > fuzz/in/seed
afl-fuzz -i fuzz/in -o fuzz/out -- build-fuzz-afl/fuzz/wasm/wasm_fuzzer -t3000
```

`--privileged` is needed so that AFL can adjust `/proc/sys/kernel/core_pattern`
and disable ASLR inside the container. Without it AFL prints warnings but still
runs (use `AFL_SKIP_CPUFREQ=1` and `AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1` if
you want to skip the checks instead).

The conan named volume (`rippled-fuzz-conan`) caches downloaded packages so
that the `conan install` step inside `build.sh` is fast on subsequent runs.

# Running
Executing a fuzzing campaign is straightforward.

## General WASM Fuzzer

```
./build.sh afl
mkdir in
echo "1111111111111111111111111111111111111111111111" in/in
afl-fuzz -i ./in -o ./out ../build-fuzz-afl/fuzz/wasm/wasm_fuzzer -t3000
```

This will spawn a fuzzer on one core. We recommend using at least 16 cores when fuzzing. 

Please see this section of AFL++'s documentation on how to use [multiple cores](https://github.com/AFLplusplus/AFLplusplus/blob/stable/docs/fuzzing_in_depth.md#c-using-multiple-cores).

### Sanitizers
As the host functions are writting in C++ we recommend using ASAN (Address Sanitizer) and UBSAN (Undefined Behavior Sanitizer) when fuzzing.
You can compile both harnesses with ASAN and UBSAN with ``build.sh``. Then you can run the fuzzer as you would normally but with less cores and with syncing.

```
# ASAN
./build.sh asan
file ../build-fuzz-asan/fuzz/wasm/wasm_fuzzer

./build.sh ubsan
file ../build-fuzz-host-asan/fuzz/wasm/wasm_fuzzer

# UBSAN
./build.sh host asan
file ../build-fuzz-ubsan/fuzz/wasm/wasm_fuzzer
./build.sh ubsan
file ../build-fuzz-host-ubsan/fuzz/wasm/wasm_fuzzer
```

## Host Function Fuzzer

```
./build.sh host afl
cd autarkie-fuzzer
cargo build --release
./target/release/autarkie-fuzzer -c0-$(nproc) -o ./out . ../build-fuzz-afl/fuzz/wasm/wasm_fuzzer -t3000 -r
```

This will run the grammar fuzzer on all available cores. 

If you'd like to customize the number of cores, use ``-c0-xx`` where xx is the maximum number of cores you'd like to use.

# Coverage

After fuzzing for a while, you can check the fuzzer's progress by getting the code coverage of its test cases.
Here are the steps to fetch coverage:

## Coverage for General WASM Fuzzer
```
./build.sh coverage
../build-fuzz-coverage/fuzz/wasm/wasm_fuzzer -runs=0 -print-coverage=1 <afl output directory>/mainaflfuzzer/queue/
llvm-profdata-21 merge default.profraw -o ./default.profdata
llvm-cov-21 show -format=html -instr-profile ./default.profdata ../build-fuzz-coverage/fuzz/wasm/wasm_fuzzer -o ./coverage-out
cd coverage-out
python3 -m http.server
```

## Coverage for Grammar fuzzer
```
./build.sh coverage
../build-fuzz-coverage/fuzz/wasm/wasm_fuzzer -runs=0 -print-coverage=1 <grammar output directory>/0/rendered_corpus/
llvm-profdata-21 merge default.profraw -o ./default.profdata
llvm-cov-21 show -format=html -instr-profile ./default.profdata ../build-fuzz-coverage/fuzz/wasm/wasm_fuzzer -o ./coverage-out
cd coverage-out
python3 -m http.server
```
