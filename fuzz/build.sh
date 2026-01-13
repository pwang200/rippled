#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<USAGE
Usage: $0 [host] [afl|asan|coverage]
  host      Build with FUZZ_HOST enabled
  afl       Build with afl-clang-fast instrumentation (default)
  asan      Build with clang + AddressSanitizer
  coverage  Build with clang + LLVM coverage (-fprofile-instr-generate -fcoverage-mapping)
USAGE
}

host_build=false
if [[ "${1:-}" == "host" ]]; then
  host_build=true
  shift
fi

variant=${1:-afl}
case "$variant" in
  afl|asan|coverage) ;;
  -h|--help) usage; exit 0 ;;
  *) echo "Unknown variant '$variant'" >&2; usage; exit 1 ;;
esac

setup() {
  local build_dir=$1; shift
  local build_type=$1; shift
  local c_comp=$1; shift
  local cxx_comp=$1; shift
  local -a extra_cmake=("$@")

  mkdir -p "$build_dir"
  if ! conan remote list | grep -q '^xrplf'; then
    conan remote add xrplf https://conan.ripplex.io --force
  fi
  conan install . \
    --output-folder="$build_dir" \
    -o fuzzer=True \
    -o tests=False \
    --build=missing \
    -s compiler=clang \
    -s compiler.version=21 \
    -s compiler.cppstd=20 \
    -s compiler.libcxx=libstdc++11 \
    -s build_type=$build_type
  cd ./fuzz/wasm-smith-lib
  cargo build --release
  cd ../..
  cmake -S . -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$build_dir/build/generators/conan_toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=$build_type \
    -DCMAKE_C_COMPILER=$c_comp \
    -DCMAKE_CXX_COMPILER=$cxx_comp \
    -Dxrpld=ON \
    -Dtests=OFF \
    "${extra_cmake[@]}"
}
cd ..
case "$variant" in
  afl)
    extra_cxx_flags=()
    if $host_build; then
      extra_cxx_flags+=("-DCMAKE_CXX_FLAGS=-DFUZZ_HOST")
    fi
    setup build-afl Release afl-clang-fast afl-clang-fast++ \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
      -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
      "${extra_cxx_flags[@]}"
    AFL_LLVM_CMPLOG=1 AFL_LLVM_ALLOWLIST=$(pwd)/fuzz/wasm/afl_allowlist.txt cmake --build build-afl --target wasm_fuzzer -j"$(nproc)"
    ;;
  asan)
    cxx_flags="-fsanitize=undefined"
    if $host_build; then
      cxx_flags+=" -DFUZZ_HOST"
    fi
    setup build-ubsan Debug afl-clang-fast afl-clang-fast++ \
      "-DCMAKE_CXX_FLAGS=${cxx_flags}" \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
      -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
      AFL_LLVM_CMPLOG=1 AFL_LLVM_ALLOWLIST=$(pwd)/fuzz/wasm/afl_allowlist.txt cmake --build build-ubsan --target wasm_fuzzer -j"$(nproc)"
    ;;
  coverage)
    extra_cxx_flags=()
    if $host_build; then
      extra_cxx_flags+=("-DCMAKE_CXX_FLAGS=-DFUZZ_HOST")
    fi
    setup build-cov-release Release clang clang++ \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
      -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
      "${extra_cxx_flags[@]}"
    cmake --build build-cov-release --target wasm_fuzzer -j"$(nproc)"
    ;;
esac
