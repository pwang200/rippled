#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<USAGE
Usage: $0 [host] [afl|ubsan|asan|coverage]
  host      Build with FUZZ_HOST enabled
  afl       Build with afl-clang-fast instrumentation (default)
  asan      Build with afl-clang-fast + AddressSanitizer
  ubsan     Build with afl-clang-fast + UndefinedBehaviorSanitizer
  coverage  Build with clang + LLVM coverage (-fprofile-instr-generate -fcoverage-mapping)
USAGE
}

# OS detection
OS="$(uname -s)"
case "$OS" in
  Linux)
    LIBCXX="libstdc++11"
    NPROC="$(nproc)"
    CLANG_BIN="clang-21"
    CLANGXX_BIN="clang++-21"
    # Tell afl-clang-fast to use clang-21 instead of its default (Ubuntu package
    # ships with clang-17). This must match the version used to build conan deps.
    AFL_ENV=("AFL_CC=clang-21" "AFL_CXX=clang++-21")
    LINKER_FLAGS=("-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld-21" "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld-21")
    ;;
  Darwin)
    LIBCXX="libc++"
    NPROC="$(sysctl -n hw.ncpu)"
    # On macOS, brew's afl++ wraps its own clang; point it at clang-21 from llvm@21
    LLVM21="/usr/local/opt/llvm@21/bin"
    CLANG_BIN="${LLVM21}/clang-21"
    CLANGXX_BIN="${LLVM21}/clang++"
    AFL_ENV=("AFL_CC=${LLVM21}/clang-21" "AFL_CXX=${LLVM21}/clang++")
    # macOS uses its own linker (ld64); lld is Linux-specific
    LINKER_FLAGS=()
    ;;
  *)
    echo "Unsupported OS: $OS" >&2
    exit 1
    ;;
esac

host_build=false
if [[ "${1:-}" == "host" ]]; then
  host_build=true
  shift
fi

variant=${1:-afl}
case "$variant" in
  afl|asan|ubsan|coverage) ;;
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
    -s compiler.libcxx=${LIBCXX} \
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
    "${extra_cmake[@]+"${extra_cmake[@]}"}"
}
cd ..
case "$variant" in
  afl)
    extra_cxx_flags=()
    if $host_build; then
      extra_cxx_flags+=("-DCMAKE_CXX_FLAGS=-DFUZZ_HOST")
    fi
    build_dir=build-fuzz-afl
    if $host_build; then
      build_dir=build-fuzz-host-afl
    fi
    setup "$build_dir" Debug afl-clang-fast afl-clang-fast++ \
      "${LINKER_FLAGS[@]+"${LINKER_FLAGS[@]}"}" \
      "${extra_cxx_flags[@]+"${extra_cxx_flags[@]}"}"
    env "${AFL_ENV[@]+"${AFL_ENV[@]}"}" \
      AFL_LLVM_CMPLOG=1 AFL_LLVM_ALLOWLIST=$(pwd)/fuzz/wasm/afl_allowlist.txt \
      cmake --build "$build_dir" --target wasm_fuzzer -j"${NPROC}"
    ;;
  asan)
    cxx_flags="-fsanitize=address"
    if $host_build; then
      cxx_flags+=" -DFUZZ_HOST"
    fi
    build_dir=build-fuzz-asan
    if $host_build; then
      build_dir=build-fuzz-host-asan
    fi
    setup "$build_dir" Debug afl-clang-fast afl-clang-fast++ \
      "-DCMAKE_CXX_FLAGS=${cxx_flags}" \
      "${LINKER_FLAGS[@]+"${LINKER_FLAGS[@]}"}"
    env "${AFL_ENV[@]+"${AFL_ENV[@]}"}" \
      AFL_LLVM_CMPLOG=1 AFL_LLVM_ALLOWLIST=$(pwd)/fuzz/wasm/afl_allowlist.txt \
      cmake --build "$build_dir" --target wasm_fuzzer -j"${NPROC}"
    ;;
  ubsan)
    cxx_flags="-fsanitize=undefined"
    if $host_build; then
      cxx_flags+=" -DFUZZ_HOST"
    fi
    build_dir=build-fuzz-ubsan
    if $host_build; then
      build_dir=build-fuzz-host-ubsan
    fi
    setup "$build_dir" Debug afl-clang-fast afl-clang-fast++ \
      "-DCMAKE_CXX_FLAGS=${cxx_flags}" \
      "${LINKER_FLAGS[@]+"${LINKER_FLAGS[@]}"}"
    env "${AFL_ENV[@]+"${AFL_ENV[@]}"}" \
      AFL_LLVM_CMPLOG=1 AFL_LLVM_ALLOWLIST=$(pwd)/fuzz/wasm/afl_allowlist.txt \
      cmake --build "$build_dir" --target wasm_fuzzer -j"${NPROC}"
    ;;
  coverage)
    extra_cxx_flags=()
    if $host_build; then
      extra_cxx_flags+=("-DCMAKE_CXX_FLAGS=-DFUZZ_HOST")
    fi
    build_dir=build-fuzz-coverage
    if $host_build; then
      build_dir=build-fuzz-host-coverage
    fi
    setup "$build_dir" Release "${CLANG_BIN}" "${CLANGXX_BIN}" \
      "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping" \
      "${LINKER_FLAGS[@]+"${LINKER_FLAGS[@]}"}" \
      "${extra_cxx_flags[@]+"${extra_cxx_flags[@]}"}"
    cmake --build "$build_dir" --target wasm_fuzzer -j"${NPROC}"
    ;;
esac
