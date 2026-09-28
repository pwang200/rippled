#include <xrpl/protocol/TER.h>
#include <xrpl/tx/wasm/WasmCommon.h>
#include <xrpl/tx/wasm/WasmVM.h>

#include <gtest/gtest.h>
#include <tx/wasm/fixtures/RealHostFixture.h>
#include <tx/wasm/fixtures/WasmRun.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

// A one-off timing driver, not a correctness suite: it runs hand-written `.wat` modules from
// `timing/wat/` through the real VM against a real host under a fixed fuel budget, and prints
// wall time plus gas consumed so a host function's cost can be read off directly. More `.wat`
// files land in `timing/wat/` over time; each gets its own TEST_F, sharing the timing loop
// below.
//
// Every run pays a fixed cost to compile, instantiate, and dispatch the module before the
// guest's first instruction (see `crates/xrpl-wasm-vm/src/vm.rs`'s note that this happens
// before any gas charge can reach it) — on this machine, measured at `BaselineNoHostCalls`
// below, that is on the order of 13 microseconds, which is *not* negligible next to a single
// call's cost. Every case here subtracts it, the same way `benchmarkThroughVm` in
// `src/benchmarks/libxrpl/wasm/WasmBench.h` subtracts a `count = 0` baseline, rather than
// reporting a raw total divided by a call count.

namespace xrpl::test {
namespace {

std::string
readTextFile(std::filesystem::path const& path)
{
    std::ifstream in{path};
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void
writeBinaryFile(std::filesystem::path const& path, Bytes const& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

struct HostFunctionTiming : RealHostFixture
{
    static std::filesystem::path
    watDir()
    {
        return std::filesystem::path{__FILE__}.parent_path() / "wat";
    }

    struct Timing
    {
        double bestSeconds{};
        std::int64_t gasConsumed{};
    };

    // Runs `wasm`'s `escrow_finish` under `gasLimit`, `repeats` times, keeping the fastest —
    // noise only ever adds delay, so the minimum is the closest a wall clock gets to "what the
    // engine actually costs" on a shared, unpinned machine. A fresh host every time: a host
    // serves exactly one run, since `runEscrowWasm` asserts it was handed a clean one
    // (`checkSelf`).
    //
    // `allowNegativeResult`: some cases (e.g. a deliberate "object not found" run) expect every
    // call to answer a negative `HostFunctionError` code, which is the point of the run, not a
    // bug in it.
    Timing
    time(
        Bytes const& wasm,
        std::int64_t gasLimit,
        int repeats = 10,
        bool allowNegativeResult = false)
    {
        auto bestSeconds = std::numeric_limits<double>::infinity();
        std::int64_t gasConsumed = 0;
        for (int i = 0; i < repeats; ++i)
        {
            auto host = makeHost();
            auto const start = std::chrono::steady_clock::now();
            auto const outcome = runEscrowWasm(wasm, *host, gasLimit);
            auto const elapsed = std::chrono::steady_clock::now() - start;

            if (!outcome.has_value())
            {
                ADD_FAILURE() << "the run did not complete: " << transToken(outcome.error().ter);
                return {};
            }
            if (!allowNegativeResult)
                EXPECT_GE(outcome->result, 0)
                    << "the contract's host call returned error code " << outcome->result;

            gasConsumed = outcome->cost;
            bestSeconds = std::min(bestSeconds, std::chrono::duration<double>(elapsed).count());
        }
        return {bestSeconds, gasConsumed};
    }
};

TEST_F(HostFunctionTiming, Sha512HalfLoop)
{
    constexpr std::int64_t gasLimit = 1'000'000;
    constexpr int callCount = 496;

    auto const wasm = assembleWat(readTextFile(watDir() / "sha512_half_loop.wat"));
    writeBinaryFile(watDir().parent_path() / "wasm" / "sha512_half_loop.wasm", wasm);
    auto const baselineWasm = assembleWat(readTextFile(watDir() / "baseline_no_host_calls.wat"));

    auto const baseline = time(baselineWasm, gasLimit);
    auto const loaded = time(wasm, gasLimit);
    auto const perCallSeconds =
        std::max(0.0, loaded.bestSeconds - baseline.bestSeconds) / callCount;

    std::cout << "sha512_half loop: raw best " << (loaded.bestSeconds * 1e9) << " ns over "
               << callCount << " calls (gas consumed " << loaded.gasConsumed
               << " of " << gasLimit << " budget); baseline (compile+instantiate, no host calls) "
               << (baseline.bestSeconds * 1e9) << " ns; baseline-corrected ~"
               << (perCallSeconds * 1e9) << " ns/call\n";
}

// Sanity-checks the handoff soak probe (cache_ledger_obj_soak.wat): assembles and runs it
// against a real host/tx, confirming it completes within budget and reports the actual gas
// consumed, rather than trusting the by-hand fuel estimate in the wat's own comment.
TEST_F(HostFunctionTiming, CacheLedgerObjSoakProbe)
{
    constexpr std::int64_t gasLimit = 1'000'000;

    auto const watText = readTextFile(watDir() / "cache_ledger_obj_soak.wat");
    auto const wasm = assembleWat(watText);
    writeBinaryFile(watDir().parent_path() / "wasm" / "cache_ledger_obj_soak.wasm", wasm);

    // Uniformly-random 32-byte keys essentially never coincide with a real ledger object
    // (10M objects in a 2^256 space, or a handful in this tiny test fixture — either way the
    // collision probability is negligible), so every one of the 142 calls is expected to
    // answer `LedgerObjNotFound`, not a hit. That's the correct outcome for this probe, not a
    // bug — see the discussion this wat came out of for why "found" needs a different design.
    auto const result = time(wasm, gasLimit, 10, /* allowNegativeResult */ true);

    std::cout << "cache_ledger_obj soak probe: best of 10 runs = " << (result.bestSeconds * 1e9)
               << " ns, gas consumed = " << result.gasConsumed << " of " << gasLimit << "\n";
}

// Isolates the fixed cost of compiling, instantiating, and running a module to completion
// with zero host calls inside — what `Sha512HalfLoop` (and every future case here) subtracts
// out rather than folding into its per-call estimate.
TEST_F(HostFunctionTiming, BaselineNoHostCalls)
{
    auto const wasm = assembleWat(readTextFile(watDir() / "baseline_no_host_calls.wat"));
    auto const result = time(wasm, 1'000'000);

    std::cout << "baseline (no host calls): best of 10 runs = " << (result.bestSeconds * 1e9)
               << " ns, gas consumed = " << result.gasConsumed << "\n";
}

}  // namespace xrpl::test
