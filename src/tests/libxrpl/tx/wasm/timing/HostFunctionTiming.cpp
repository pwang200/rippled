#include <xrpl/beast/utility/Journal.h>
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
#include <iterator>
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

Bytes
readBinaryFile(std::filesystem::path const& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    return Bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
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
    Timing
    time(Bytes const& wasm, std::int64_t gasLimit, int repeats = 10)
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
            EXPECT_EQ(outcome->result, 1) << "the contract did not return 1";

            gasConsumed = outcome->cost;
            bestSeconds = std::min(bestSeconds, std::chrono::duration<double>(elapsed).count());
        }
        return {bestSeconds, gasConsumed};
    }

    void
    runSha512HalfLoop(std::string const& name, std::int64_t gasLimit, int callCount)
    {
        auto const wasm = assembleWat(readTextFile(watDir() / (name + ".wat")));
        writeBinaryFile(watDir().parent_path() / "wasm" / (name + ".wasm"), wasm);
        auto const baselineWasm = assembleWat(readTextFile(watDir() / "return_1.wat"));

        auto const baseline = time(baselineWasm, gasLimit);
        auto const loaded = time(wasm, gasLimit);
        auto const perCallSeconds =
            std::max(0.0, loaded.bestSeconds - baseline.bestSeconds) / callCount;

        std::cout << name << ": raw best " << (loaded.bestSeconds * 1e9) << " ns over "
                   << callCount << " calls (gas consumed " << loaded.gasConsumed
                   << " of " << gasLimit << " budget); baseline (compile+instantiate, no host calls) "
                   << (baseline.bestSeconds * 1e9) << " ns; baseline-corrected ~"
                   << (perCallSeconds * 1e9) << " ns/call\n";
    }

    void
    runCacheLedgerObjSoak(std::string const& name, std::int64_t gasLimit)
    {
        auto const wasm = assembleWat(readTextFile(watDir() / (name + ".wat")));
        writeBinaryFile(watDir().parent_path() / "wasm" / (name + ".wasm"), wasm);

        auto const result = time(wasm, gasLimit);

        std::cout << name << ": best of 10 runs = " << (result.bestSeconds * 1e9)
                   << " ns, gas consumed = " << result.gasConsumed << " of " << gasLimit << "\n";
    }
};

// The `.wasm` files in `timing/wasm/` are handed to other people as-is, so this runs the
// committed bytes themselves (not a fresh assembly of the `.wat`): each must be exactly what
// the `.wat` assembles to, pass the preflight check EscrowCreate applies to bytecode, run to
// completion under its budget, return 1, and use at least `minGas` (most of the budget for the
// loops).
TEST_F(HostFunctionTiming, CommittedWasmFilesRun)
{
    struct Case
    {
        char const* name;
        std::int64_t gasLimit;
        std::int64_t minGas;
    };
    for (auto const& c : {
             Case{"return_1", 1'000'000, 1},
             Case{"sha512_half_loop", 1'000'000, 980'000},
             Case{"sha512_half_loop_400k", 400'000, 392'000},
             Case{"cache_ledger_obj_soak", 1'000'000, 980'000},
             Case{"cache_ledger_obj_soak_400k", 400'000, 392'000},
         })
    {
        SCOPED_TRACE(c.name);
        auto const wasm =
            readBinaryFile(watDir().parent_path() / "wasm" / (std::string{c.name} + ".wasm"));
        EXPECT_EQ(wasm, assembleWat(readTextFile(watDir() / (std::string{c.name} + ".wat"))))
            << "the committed .wasm is stale relative to its .wat";

        EXPECT_EQ(
            preflightEscrowWasm(wasm, beast::Journal{beast::Journal::getNullSink()}),
            tesSUCCESS)
            << "EscrowCreate would refuse this bytecode";

        auto host = makeHost();
        auto const outcome = runEscrowWasm(wasm, *host, c.gasLimit);
        ASSERT_TRUE(outcome.has_value()) << transToken(outcome.error().ter);
        EXPECT_EQ(outcome->result, 1);
        EXPECT_LE(outcome->cost, c.gasLimit);
        EXPECT_GE(outcome->cost, c.minGas);
    }
}

// The `call_indirect_unroll*` modules come from the wasmi `module-new-time` experiment
// (`real_total/t-b-*`): `case.wasm` is generated there, and the `.wat` here is only its sketch
// (it elides the repeated parts), so there is no assembly to compare with. The experiment
// exports `finish`; the export in these files was renamed to `escrow_finish`, the name
// EscrowCreate requires. They import nothing. Each must pass preflight, run to completion
// under its budget, return 1, and use most of it.
TEST_F(HostFunctionTiming, CallIndirectUnrollRuns)
{
    struct Case
    {
        char const* name;
        std::int64_t gasLimit;
        std::int64_t minGas;
    };
    for (auto const& c : {
             Case{"call_indirect_unroll", 1'000'000, 930'000},
             Case{"call_indirect_unroll_400k", 400'000, 370'000},
         })
    {
        SCOPED_TRACE(c.name);
        auto const wasm =
            readBinaryFile(watDir().parent_path() / "wasm" / (std::string{c.name} + ".wasm"));

        EXPECT_EQ(
            preflightEscrowWasm(wasm, beast::Journal{beast::Journal::getNullSink()}),
            tesSUCCESS)
            << "EscrowCreate would refuse this bytecode";

        auto host = makeHost();
        auto const start = std::chrono::steady_clock::now();
        auto const outcome = runEscrowWasm(wasm, *host, c.gasLimit);
        auto const elapsed = std::chrono::steady_clock::now() - start;
        ASSERT_TRUE(outcome.has_value()) << transToken(outcome.error().ter);
        EXPECT_EQ(outcome->result, 1);
        EXPECT_LE(outcome->cost, c.gasLimit);
        EXPECT_GE(outcome->cost, c.minGas);

        std::cout << c.name << ": " << wasm.size() << " bytes, gas consumed " << outcome->cost
                  << " of " << c.gasLimit << ", one run "
                  << std::chrono::duration<double>(elapsed).count() * 1e3 << " ms\n";
    }
}

TEST_F(HostFunctionTiming, Sha512HalfLoop)
{
    runSha512HalfLoop("sha512_half_loop", 1'000'000, 496);
}

TEST_F(HostFunctionTiming, Sha512HalfLoop400K)
{
    runSha512HalfLoop("sha512_half_loop_400k", 400'000, 198);
}

// Sanity-checks the handoff soak probe (cache_ledger_obj_soak.wat): assembles and runs it
// against a real host/tx, confirming it completes within budget and reports the actual gas
// consumed, rather than trusting the by-hand fuel estimate in the wat's own comment.
//
// Uniformly-random 32-byte keys essentially never coincide with a real ledger object (10M
// objects in a 2^256 space, or a handful in this tiny test fixture — either way the collision
// probability is negligible), so every `cache_le` call is expected to answer
// `LedgerObjNotFound`, not a hit. The contract ignores that code and returns 1.
TEST_F(HostFunctionTiming, CacheLedgerObjSoakProbe)
{
    runCacheLedgerObjSoak("cache_ledger_obj_soak", 1'000'000);
}

TEST_F(HostFunctionTiming, CacheLedgerObjSoakProbe400K)
{
    runCacheLedgerObjSoak("cache_ledger_obj_soak_400k", 400'000);
}

// Isolates the fixed cost of compiling, instantiating, and running a module to completion
// with zero host calls inside — what `Sha512HalfLoop` (and every future case here) subtracts
// out rather than folding into its per-call estimate.
TEST_F(HostFunctionTiming, BaselineNoHostCalls)
{
    auto const wasm = assembleWat(readTextFile(watDir() / "return_1.wat"));
    writeBinaryFile(watDir().parent_path() / "wasm" / "return_1.wasm", wasm);
    auto const result = time(wasm, 1'000'000);

    std::cout << "baseline (no host calls): best of 10 runs = " << (result.bestSeconds * 1e9)
               << " ns, gas consumed = " << result.gasConsumed << "\n";
}

}  // namespace xrpl::test
