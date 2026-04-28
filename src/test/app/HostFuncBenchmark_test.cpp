// Microbenchmarks for host-function wrapper costs.
//
// These tests are MANUAL: they are not run by the regular `rippled --unittest`
// pass. Invoke explicitly, e.g.
//
//   rippled --unittest=HostFuncBenchmark
//
// They time wrapper-level work (chargeCall + getDataXxx + impl + returnResult)
// in tight loops, exercising the REAL WasmHostFunctionsImpl (not the test
// stub). They deliberately do NOT go through the WasmEngine -- per-call wasm
// dispatch / instruction execution is excluded so the numbers match what the
// per-function fixed `gas` constants were chosen to cover.
//
// Output is printed via the suite log; there are no functional assertions
// beyond a single sanity check that the wrapper returns success.

#include <test/jtx.h>

#include <xrpl/ledger/OpenView.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/tx/wasm/HostFuncImpl.h>
#include <xrpl/tx/wasm/HostFuncWrapper.h>
#include <xrpl/tx/wasm/ParamsHelper.h>

#include <wasm.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace xrpl {
namespace test {

// ---------------------------------------------------------------------------
// Stub WasmRuntimeWrapper used by the wrappers under measurement.
// Provides a small host-side memory buffer (for setData writes) and a
// per-call gas counter.
// ---------------------------------------------------------------------------
class BenchRuntime : public WasmRuntimeWrapper
{
    std::vector<std::uint8_t> mem_;
    std::int64_t gas_ = 0;

public:
    explicit BenchRuntime(std::size_t memSize) : mem_(memSize)
    {
    }

    void
    resetGas(std::int64_t g)
    {
        gas_ = g;
    }

    wmem
    getMem() override
    {
        return wmem{mem_.data(), mem_.size()};
    }

    std::int64_t
    getGas() override
    {
        return gas_;
    }

    std::int64_t
    setGas(std::int64_t g) override
    {
        gas_ = g;
        return gas_;
    }
};

struct HostFuncBenchmark_test : public beast::unit_test::suite
{
    // Each "sample" times kInner consecutive wrapper calls and divides by
    // kInner. This amortizes std::chrono::steady_clock granularity (~30-45 ns
    // on macOS) across many calls so a per-call estimate is well-resolved.
    // kSamples is the number of such samples used for statistics (median, IQR).
    static constexpr int kInner = 1000;
    static constexpr int kSamples = 1000;
    static constexpr int kWarmupSamples = 5;
    // Per-sample gas budget. Chosen large enough that the chargeCall draws
    // (one per wrapper call, gas 60 each at present) cannot trip OOG within
    // a sample, with a wide safety margin for future per-call gas changes.
    static constexpr std::int64_t kGasBudgetPerSample = 1'000'000'000;

    void
    testGetLedgerSqnBaseline()
    {
        testcase("getLedgerSqn wrapper baseline (real impl)");

        using namespace jtx;
        Env env{*this};

        // Real-impl plumbing: dummy STTx (ESCROW_FINISH, empty body) and
        // dummy Keylet -- getLedgerSqn does not use them, only ctx_.view().
        OpenView ov{*env.current()};
        STTx const tx(ttESCROW_FINISH, [](STObject&) {});
        ApplyContext ac{
            env.app(),
            ov,
            tx,
            tesSUCCESS,
            env.current()->fees().base,
            tapNONE,
            env.journal};
        auto const dummyEscrow = keylet::escrow(env.master, env.seq(env.master));
        WasmHostFunctionsImpl hfs(ac, dummyEscrow);

        // Stub runtime large enough for the 4-byte uint32 output.
        BenchRuntime rt(/*memSize*/ 64);
        hfs.setRT(&rt);

        // The wrapper reads `gas` off impFunc via udata->second.gas.
        // We use the production constant (60) so the calibration is "ns per
        // gas unit, anchored at the existing cost for getLedgerSqn".
        WasmImportFunc impFunc;
        impFunc.name = "get_ledger_sqn";
        impFunc.gas = 60;

        WasmUserData udata{&hfs, impFunc};

        // params: (out_ptr i32, out_len i32) -- where the wrapper writes the
        // 4-byte little-endian uint32 ledger sequence into stub memory.
        wasm_val_t paramsData[2]{};
        paramsData[0].kind = WASM_I32;
        paramsData[0].of.i32 = 0;  // offset into BenchRuntime::mem_
        paramsData[1].kind = WASM_I32;
        paramsData[1].of.i32 = 4;  // bytes available

        wasm_val_vec_t params{};
        params.size = 2;
        params.data = paramsData;

        // results: one i32 slot for the wrapper to write its host-side return
        // code into (via hfResult).
        wasm_val_t resultsData[1]{};
        resultsData[0].kind = WASM_I32;

        wasm_val_vec_t results{};
        results.size = 1;
        results.data = resultsData;

        // Sanity: wrapper actually works once before we start timing.
        rt.resetGas(kGasBudgetPerSample);
        auto* trap = getLedgerSqn_wrap(&udata, &params, &results);
        BEAST_EXPECT(trap == nullptr);

        // Warmup -- prime icache, branch predictors, and the OS scheduler.
        for (int i = 0; i < kWarmupSamples; ++i)
        {
            rt.resetGas(kGasBudgetPerSample);
            for (int j = 0; j < kInner; ++j)
                getLedgerSqn_wrap(&udata, &params, &results);
        }

        // Timed loop. Each outer iteration is one "sample" = kInner consecutive
        // wrapper calls timed together; we divide elapsed by kInner to get a
        // per-call ns estimate that's well above clock granularity.
        std::vector<std::int64_t> samples;
        samples.reserve(kSamples);
        for (int i = 0; i < kSamples; ++i)
        {
            rt.resetGas(kGasBudgetPerSample);
            auto const t0 = std::chrono::steady_clock::now();
            for (int j = 0; j < kInner; ++j)
                getLedgerSqn_wrap(&udata, &params, &results);
            auto const t1 = std::chrono::steady_clock::now();
            auto const elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count();
            samples.push_back(elapsed / kInner);
        }

        std::sort(samples.begin(), samples.end());
        auto const median = samples[samples.size() / 2];
        auto const p25 = samples[samples.size() / 4];
        auto const p75 = samples[(samples.size() * 3) / 4];
        auto const iqr = p75 - p25;

        double const nsPerGas =
            static_cast<double>(median) / static_cast<double>(impFunc.gas);

        log << "\n";
        log << "=== getLedgerSqn wrapper baseline ===\n";
        log << "  samples:          " << kSamples << " (each = " << kInner
            << " calls amortized)\n";
        log << "  median ns/call:   " << median << "\n";
        log << "  p25 / p75 ns:     " << p25 << " / " << p75 << "\n";
        log << "  IQR / median:     " << (100.0 * iqr / median) << "%\n";
        log << "  per-call gas:     " << impFunc.gas << "\n";
        log << "  ns per gas unit:  " << nsPerGas << "\n";
        log << std::endl;
    }

    void
    run() override
    {
        testGetLedgerSqnBaseline();
    }
};

BEAST_DEFINE_TESTSUITE_MANUAL(HostFuncBenchmark, app, xrpl);

}  // namespace test
}  // namespace xrpl
