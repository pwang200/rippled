# AFL++ fuzzing environment for rippled WASM fuzzer
#
# Usage:
#   Build the image (once):
#     docker build -t rippled-fuzz fuzz/
#
#   Run a fuzzing session (mount the repo as /src):
#     docker run --rm -it \
#       -v "$(pwd):/src" \
#       -v rippled-fuzz-conan:/root/.conan2 \
#       rippled-fuzz
#
#   Inside the container, build and run:
#     cd /src && ./fuzz/build.sh afl
#     afl-fuzz -i fuzz/in -o fuzz/out -- build-fuzz-afl/wasm_fuzzer @@
#
# Named volume "rippled-fuzz-conan" persists the conan package cache across
# container runs so that `conan install` only downloads packages once.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# ── System packages ──────────────────────────────────────────────────────────

RUN apt-get update && apt-get install -y --no-install-recommends \
    # build essentials
    build-essential \
    ninja-build \
    pkg-config \
    git \
    curl \
    wget \
    ca-certificates \
    gnupg \
    lsb-release \
    python3 \
    python3-pip \
    python3-venv \
    # for conan
    python3-setuptools \
    && rm -rf /var/lib/apt/lists/*

# ── CMake (recent) ───────────────────────────────────────────────────────────

RUN pip3 install --break-system-packages cmake

# ── LLVM / Clang 21 ─────────────────────────────────────────────────────────
# Install via the official LLVM apt repo so we get the exact version used
# in build.sh (-s compiler.version=21).

RUN wget -qO - https://apt.llvm.org/llvm-snapshot.gpg.key \
      | gpg --dearmor -o /usr/share/keyrings/llvm.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] \
      https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main" \
      > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && apt-get install -y --no-install-recommends \
      clang-21 \
      lld-21 \
      libc++-21-dev \
      libc++abi-21-dev \
    && update-alternatives --install /usr/bin/clang   clang   /usr/bin/clang-21   100 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-21 100 \
    && update-alternatives --install /usr/bin/cc      cc      /usr/bin/clang-21   100 \
    && update-alternatives --install /usr/bin/c++     c++     /usr/bin/clang++-21 100 \
    && rm -rf /var/lib/apt/lists/*

# Make clang-21 the default compiler for conan and cmake
ENV CC=clang-21
ENV CXX=clang++-21

# ── AFL++ (built from source against LLVM 21) ────────────────────────────────
# The Ubuntu apt package ships AFL++ compiled against LLVM 17. Its LLVM pass
# plugins (cmplog, SanitizerCoveragePCGUARD, etc.) cannot be loaded by
# clang-21 because the LLVM API changed. Building from source against LLVM 21
# ensures the plugins and the compiler are ABI-compatible.

RUN apt-get update && apt-get install -y --no-install-recommends \
      llvm-21-dev \
      llvm-21 \
      libclang-rt-21-dev \
      make \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth=1 https://github.com/AFLplusplus/AFLplusplus /afl && \
    cd /afl && \
    LLVM_CONFIG=llvm-config-21 CC=clang-21 CXX=clang++-21 \
      make -j$(/usr/bin/nproc) && \
    make install && \
    rm -rf /afl

ENV PATH="/usr/local/bin:${PATH}"

# Sanity check: confirm afl-clang-fast uses clang-21
RUN afl-clang-fast --version 2>&1 | grep -q "clang version 21" && echo "OK: afl-clang-fast uses clang-21"

# ── Rust / Cargo ─────────────────────────────────────────────────────────────
# wasm-smith-lib is a Rust crate built as part of the fuzzer.

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --default-toolchain stable
ENV PATH="/root/.cargo/bin:${PATH}"

# ── Conan 2 ──────────────────────────────────────────────────────────────────

RUN pip3 install --break-system-packages conan && \
    # Initialize conan config directories, then override default profile to clang-21.
    # conan profile detect auto-detects GCC (from build-essential) which we do not want.
    conan profile detect --force && \
    # Detect architecture: map dpkg arch names to Conan arch names and pick
    # the right C++ stdlib.  On arm64 clang cannot reliably find GCC's
    # libstdc++ headers, so we use clang's own libc++ instead.
    DPKG_ARCH="$(dpkg --print-architecture)" && \
    case "$DPKG_ARCH" in \
      amd64) CONAN_ARCH="x86_64"; CONAN_LIBCXX="libstdc++11" ;; \
      arm64) CONAN_ARCH="armv8";  CONAN_LIBCXX="libc++" ;; \
      *) echo "Unsupported architecture: $DPKG_ARCH" >&2; exit 1 ;; \
    esac && \
    printf "[settings]\narch=${CONAN_ARCH}\nbuild_type=Release\ncompiler=clang\ncompiler.cppstd=20\ncompiler.libcxx=${CONAN_LIBCXX}\ncompiler.version=21\nos=Linux\n" \
      > ~/.conan2/profiles/default && \
    # Add the xrplf remote used by rippled's conanfile.py
    conan remote add xrplf https://conan.ripplex.io --force

# ── AFL system config ────────────────────────────────────────────────────────
# afl-system-config sets kernel parameters at runtime; we prime the ones that
# can be set in a container via /proc (core_pattern, etc.).
# The rest (ASLR) may require --privileged or --security-opt.

RUN echo "kernel.core_pattern=core" >> /etc/sysctl.conf || true

# ── Workdir and entrypoint ───────────────────────────────────────────────────
# The repo is expected to be mounted at /src at runtime.
# The container drops you into a ready shell so you can run build.sh and
# afl-fuzz directly.

WORKDIR /src

CMD ["/bin/bash"]
