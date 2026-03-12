cmake_minimum_required(VERSION 3.16)

# Fuzzer executable
add_executable(wasm_fuzzer
    fuzz.cc
)

# Reuse xrpld core objects so we don't rebuild the daemon twice
target_link_libraries(wasm_fuzzer PRIVATE xrpld_core)

add_library(fuzzer_smith SHARED IMPORTED)
if(APPLE)
    set(FUZZER_SMITH_EXT "dylib")
elseif(WIN32)
    set(FUZZER_SMITH_EXT "dll")
else()
    set(FUZZER_SMITH_EXT "so")
endif()
set_target_properties(fuzzer_smith PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-smith-lib/target/release/libfuzzer_smith.${FUZZER_SMITH_EXT}"
)
target_link_libraries(wasm_fuzzer PRIVATE fuzzer_smith)


# Pull in the jtx helpers Env relies on, but skip heavy suites we don't need
file(GLOB jtx_impl_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/test/jtx/impl/*.cpp"
)
list(FILTER jtx_impl_sources EXCLUDE REGEX "/mpt\\.cpp$")
target_sources(wasm_fuzzer PRIVATE ${jtx_impl_sources})

# Enable tests flag
target_compile_definitions(wasm_fuzzer PRIVATE ENABLE_TESTS)
if(NOT DEFINED UNIT_TEST_REFERENCE_FEE)
    set(UNIT_TEST_REFERENCE_FEE "10")
endif()
target_compile_definitions(
    wasm_fuzzer PRIVATE UNIT_TEST_REFERENCE_FEE=${UNIT_TEST_REFERENCE_FEE}
)

# Use the same includes and links as xrpld
target_include_directories(wasm_fuzzer
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)
# Add fuzzer instrumentation
target_compile_options(wasm_fuzzer PRIVATE -fsanitize=fuzzer)
target_link_options(wasm_fuzzer PRIVATE -fsanitize=fuzzer)
if (UNIX AND NOT APPLE)
    # Protobuf's constinit globals mutate during static init; RELRO makes
    # .data.rel.ro read-only before they run, so drop RELRO for this target.
    target_link_options(wasm_fuzzer PRIVATE -Wl,-z,norelro)
endif()

# Disable unity build
set_target_properties(wasm_fuzzer PROPERTIES UNITY_BUILD OFF)
