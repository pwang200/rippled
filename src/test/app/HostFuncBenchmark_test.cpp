// Microbenchmarks for host-function wrapper costs.
//
// These tests are MANUAL: they are not run by the regular `rippled --unittest`
// pass. Invoke explicitly, e.g.
//
//   rippled --unittest=HostFuncBenchmark
//
// They time wrapper-level work (chargeCall + getDataXxx + impl + returnResult)
// in tight loops, exercising the REAL WasmHostFunctionsImpl (not the test
// stub). Most of them deliberately do NOT go through the WasmEngine --
// per-call wasm dispatch / instruction execution is excluded so the numbers
// match what the per-function fixed `gas` constants were chosen to cover.
//
// The exception is testInstructionAnchor, which DOES go through the engine
// to measure ns-per-gas in the interpreted-WASM regime -- a separate
// calibration anchor distinct from the host-fn-wrapper anchor produced by
// testGetLedgerSqnBaseline.
//
// Output is printed via the suite log; there are no functional assertions
// beyond a single sanity check that each wrapper returns success.

#include <test/jtx.h>

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/ledger/OpenView.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/tx/wasm/HostFuncImpl.h>
#include <xrpl/tx/wasm/HostFuncWrapper.h>
#include <xrpl/tx/wasm/ParamsHelper.h>
#include <xrpl/tx/wasm/WasmVM.h>

#include <boost/algorithm/hex.hpp>

#include <wasm.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace xrpl {
namespace test {

// ---------------------------------------------------------------------------
// doNotOptimize: a real compiler barrier that defeats inter-iteration
// optimization stronger than `volatile`. The "r,m" constraint forces the
// value to be materialized in a register or memory location at this exact
// point; the "memory" clobber tells the compiler all memory may have been
// modified, invalidating any cached values. Equivalent to Google
// Benchmark's DoNotOptimize.
// ---------------------------------------------------------------------------
template <typename T>
inline void
doNotOptimize(T const& value)
{
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    // Fallback for non-GCC-compatible compilers: less effective but
    // at least forces an observable read.
    static_cast<void>(reinterpret_cast<char const volatile&>(value));
#endif
}

// ---------------------------------------------------------------------------
// Stub WasmRuntimeWrapper used by the wrappers under measurement.
// Provides a host-side memory buffer (for setData / wasm-memory reads) and a
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

    std::uint8_t*
    memData()
    {
        return mem_.data();
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
    // (one per wrapper call) cannot trip OOG within a sample, with a wide
    // safety margin.
    static constexpr std::int64_t kGasBudgetPerSample = 1'000'000'000;

    // Calibration anchor populated by testGetLedgerSqnBaseline so the sweep
    // tests can convert ns-per-byte slopes into gas-per-byte estimates.
    double nsPerGas_ = 0.0;

    // Volatile sink: each timed iteration XORs an output byte (and the
    // wrapper's result code) into this sink, which prevents the compiler
    // from eliding the per-call work in tight inner loops. Without this,
    // release-build optimization can hoist or fold away byte-proportional
    // work (vector allocation, memcpy) that the benchmark is supposed to
    // measure. Volatile guarantees the read/write actually happens.
    volatile std::uint64_t benchSink_ = 0;

    struct Stats
    {
        std::int64_t median;
        std::int64_t p25;
        std::int64_t p75;
        double iqrPct;
    };

    static Stats
    computeStats(std::vector<std::int64_t> const& sortedSamples)
    {
        auto const n = sortedSamples.size();
        Stats s;
        s.median = sortedSamples[n / 2];
        s.p25 = sortedSamples[n / 4];
        s.p75 = sortedSamples[(n * 3) / 4];
        s.iqrPct =
            100.0 * static_cast<double>(s.p75 - s.p25) /
            static_cast<double>(s.median);
        return s;
    }

    template <typename Fn>
    std::vector<std::int64_t>
    runTimedLoop(
        BenchRuntime& rt,
        Fn&& fn,
        int inner = kInner,
        int samples = kSamples,
        int warmupSamples = kWarmupSamples)
    {
        // Warmup: prime icache, branch predictors, scheduler.
        for (int i = 0; i < warmupSamples; ++i)
        {
            rt.resetGas(kGasBudgetPerSample);
            for (int j = 0; j < inner; ++j)
                fn();
        }

        // Timed: `samples` outer iterations, each timing `inner` consecutive
        // calls (divided out into per-call ns).
        std::vector<std::int64_t> out;
        out.reserve(samples);
        for (int i = 0; i < samples; ++i)
        {
            rt.resetGas(kGasBudgetPerSample);
            auto const t0 = std::chrono::steady_clock::now();
            for (int j = 0; j < inner; ++j)
                fn();
            auto const t1 = std::chrono::steady_clock::now();
            auto const elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count();
            out.push_back(elapsed / inner);
        }

        std::sort(out.begin(), out.end());
        return out;
    }

    // Linear least-squares fit: returns (slope, intercept) for time = a + b*N.
    static std::pair<double, double>
    linearFit(std::vector<std::pair<double, double>> const& xy)
    {
        double const n = static_cast<double>(xy.size());
        double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
        for (auto const& [x, y] : xy)
        {
            sumX += x;
            sumY += y;
            sumXY += x * y;
            sumX2 += x * x;
        }
        double const slope = (n * sumXY - sumX * sumY) /
            (n * sumX2 - sumX * sumX);
        double const intercept = (sumY - slope * sumX) / n;
        return {slope, intercept};
    }

    // -----------------------------------------------------------------------
    // Test 1: getLedgerSqn baseline (real impl).
    // -----------------------------------------------------------------------
    void
    testGetLedgerSqnBaseline()
    {
        testcase("getLedgerSqn wrapper baseline (real impl)");

        using namespace jtx;
        Env env{*this};

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

        BenchRuntime rt(/*memSize*/ 64);
        hfs.setRT(&rt);

        WasmImportFunc impFunc;
        impFunc.name = "get_ledger_sqn";
        impFunc.gas = 60;
        WasmUserData udata{&hfs, impFunc};

        // params: (out_ptr i32, out_len i32)
        wasm_val_t paramsData[2]{};
        paramsData[0].kind = WASM_I32;
        paramsData[0].of.i32 = 0;
        paramsData[1].kind = WASM_I32;
        paramsData[1].of.i32 = 4;

        wasm_val_vec_t params{};
        params.size = 2;
        params.data = paramsData;

        wasm_val_t resultsData[1]{};
        resultsData[0].kind = WASM_I32;
        wasm_val_vec_t results{};
        results.size = 1;
        results.data = resultsData;

        rt.resetGas(kGasBudgetPerSample);
        auto* trap = getLedgerSqn_wrap(&udata, &params, &results);
        BEAST_EXPECT(trap == nullptr);

        auto const samples = runTimedLoop(rt, [&]() {
            getLedgerSqn_wrap(&udata, &params, &results);
            // Sink: read the wrapper's output byte and result code so the
            // compiler can't elide the call's work across the inner loop.
            benchSink_ ^= rt.memData()[0];
            benchSink_ ^= static_cast<std::uint64_t>(resultsData[0].of.i32);
        });
        auto const stats = computeStats(samples);

        nsPerGas_ = static_cast<double>(stats.median) /
            static_cast<double>(impFunc.gas);

        log << "\n";
        log << "=== getLedgerSqn wrapper baseline (real impl) ===\n";
        log << "  samples:          " << kSamples << " (each = " << kInner
            << " calls amortized)\n";
        log << "  median ns/call:   " << stats.median << "\n";
        log << "  p25 / p75 ns:     " << stats.p25 << " / " << stats.p75
            << "\n";
        log << "  IQR / median:     " << std::fixed << std::setprecision(2)
            << stats.iqrPct << "%\n";
        log << "  per-call gas:     " << impFunc.gas << "\n";
        log << "  ns per gas unit:  " << nsPerGas_ << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2: getCurrentLedgerObjField sweep over output size (real impl).
    // Measures the chargeMem path: the host-side Bytes copy of the SLE
    // field plus the eventual host->wasm boundary memcpy. Each SLE is
    // constructed directly in the OpenView with a synthetic sfData payload
    // of size N, so we can vary the size freely up to maxWasmDataLength
    // without going through a transaction validator.
    // -----------------------------------------------------------------------
    void
    testGetCurrentLedgerObjFieldSweep()
    {
        testcase("getCurrentLedgerObjField sweep (real impl)");

        using namespace jtx;
        Env env{*this};

        OpenView ov{*env.current()};
        STTx const tx(ttESCROW_FINISH, [](STObject&) {});
        ApplyContext ac{
            env.app(), ov, tx, tesSUCCESS, env.current()->fees().base, tapNONE, env.journal};

        // 16 KB: comfortably above the 4 KB max output + small offsets.
        BenchRuntime rt(/*memSize*/ 16 * 1024);

        WasmImportFunc impFunc;
        impFunc.name = "get_current_ledger_obj_field";
        impFunc.gas = 70;

        constexpr std::int32_t kOutOffset = 4096;

        // 4 KB is maxWasmDataLength -- the largest valid setData write.
        // Sizes intentionally reversed to diagnose whether the small-size
        // dip (128 B < 32 B) is a cold-start artifact on the first sweep
        // iteration or a real per-size effect.
        static constexpr std::array<std::int32_t, 5> sizes = {4096, 1024, 512, 128, 32};
        // 8 KB -> setData rejects (srcSize > maxWasmDataLength). The host
        // function still copies the SLE field into a Bytes value, so this
        // row reflects "host copy + bounds-check rejection", not a pure
        // fast-fail.
        constexpr std::int32_t kOversize = 8192;

        std::vector<std::pair<double, double>> sizeMedian;
        sizeMedian.reserve(sizes.size());

        log << "\n";
        log << "=== getCurrentLedgerObjField sweep (real impl) ===\n";
        log << "  output bytes |  median ns | IQR/median%\n";
        log << "  -------------+------------+-------------\n";

        auto runOne = [&](std::int32_t size, bool inBounds, std::uint32_t seq) -> Stats {
            // Unique escrow keylet per iteration so each rawInsert lands in
            // an empty slot (avoids needing rawErase between sizes).
            auto const k = keylet::escrow(env.master, seq);

            // Build a synthetic escrow SLE with sfData = `size` deterministic
            // bytes.
            auto sle = std::make_shared<SLE>(k);
            Blob payload(static_cast<std::size_t>(size), 0xAB);
            sle->setFieldVL(sfData, payload);
            ov.rawInsert(sle);

            // Fresh hfs so its cached currentLedgerObj_ resolves to *this*
            // iteration's SLE.
            WasmHostFunctionsImpl hfs(ac, k);
            hfs.setRT(&rt);

            WasmUserData udata{&hfs, impFunc};

            wasm_val_t paramsData[3]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = sfData.getCode();
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = kOutOffset;
            paramsData[2].kind = WASM_I32;
            // Output buffer length: generous enough to fit any in-bounds
            // result; for over-limit cases setData rejects regardless.
            paramsData[2].of.i32 = 8192;

            wasm_val_vec_t params{};
            params.size = 3;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            auto* trap = getCurrentLedgerObjField_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            if (inBounds)
                BEAST_EXPECT(resultsData[0].of.i32 == size);

            auto const samples = runTimedLoop(rt, [&]() {
                getCurrentLedgerObjField_wrap(&udata, &params, &results);
                // Sink: read a byte from the output region (kOutOffset) so
                // the per-byte alloc+copy work isn't elided in release.
                benchSink_ ^= rt.memData()[kOutOffset];
                benchSink_ ^=
                    static_cast<std::uint64_t>(resultsData[0].of.i32);
            });
            return computeStats(samples);
        };

        std::uint32_t seqCounter = 1000;
        for (auto const size : sizes)
        {
            auto const stats = runOne(size, /*inBounds*/ true, seqCounter++);
            std::ostringstream row;
            row << "  " << std::setw(12) << size << " | " << std::setw(10) << stats.median << " | "
                << std::fixed << std::setprecision(2) << stats.iqrPct << "%";
            log << row.str() << "\n";
            sizeMedian.emplace_back(static_cast<double>(size), static_cast<double>(stats.median));
        }

        // Over-limit row.
        {
            auto const stats = runOne(kOversize, /*inBounds*/ false, seqCounter++);
            std::ostringstream row;
            row << "  " << std::setw(12) << kOversize << " | " << std::setw(10) << stats.median
                << " | " << std::fixed << std::setprecision(2) << stats.iqrPct
                << "%   [over limit: setData rejects after host copy]";
            log << row.str() << "\n";
        }

        auto const [slope, intercept] = linearFit(sizeMedian);
        log << "  linear fit: time(N) = " << intercept << " + " << slope << " * N (ns)\n";
        log << "  intercept ns:        " << intercept << "  (existing fixed gas: " << impFunc.gas
            << ")\n";
        log << "  slope ns/byte:       " << slope << "\n";
        // "ns per gas unit" computed at the size limit (max valid output =
        // maxWasmDataLength) and at the smallest swept size. Compare against
        // the baseline calibration to see whether this wrapper is over- or
        // under-priced at each end of the size range.
        constexpr int kSmall = 32;
        constexpr int kLimit = 4096;  // maxWasmDataLength
        double const timeAtSmall = intercept + slope * static_cast<double>(kSmall);
        double const timeAtLimit = intercept + slope * static_cast<double>(kLimit);
        log << "  ns per gas unit (at N=" << kSmall
            << "): " << (timeAtSmall / static_cast<double>(impFunc.gas)) << "\n";
        log << "  ns per gas unit (at N=" << kLimit
            << "): " << (timeAtLimit / static_cast<double>(impFunc.gas)) << "\n";
        if (nsPerGas_ > 0)
        {
            log << "  intercept as gas:    " << (intercept / nsPerGas_)
                << "  (using baseline calibration " << nsPerGas_ << " ns/gas)\n";
            log << "  slope as gas/byte:   " << (slope / nsPerGas_) << "\n";
        }
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2-hardened: same as Test 2 but with stronger anti-elision
    // defenses. Per-call asm volatile compiler barriers (instead of the
    // volatile-XOR sink used elsewhere) and per-iteration cycling of the
    // out_ptr through 16 distinct destinations in BenchRuntime memory, so
    // each inner-loop iteration produces an observably different effect
    // and the compiler cannot fold identical iterations together.
    //
    // Compare slope and intercept against Test 2. If they line up
    // (within ~3-6 ns intercept difference for the defense overhead),
    // Test 2's measurement was real. If the hardened slope is much
    // larger, Test 2 was being optimized across iterations.
    // -----------------------------------------------------------------------
    void
    testGetCurrentLedgerObjFieldSweepHardened()
    {
        testcase("getCurrentLedgerObjField sweep -- HARDENED (real impl)");

        using namespace jtx;
        Env env{*this};

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

        // 16 cycling output destinations spaced 4 KB apart -> need 16*4096
        // = 64 KB minimum. Use 96 KB for headroom.
        constexpr int kNumDests = 16;
        constexpr int kDestSpacing = 4096;
        BenchRuntime rt(/*memSize*/ kNumDests * kDestSpacing + 8 * 1024);

        WasmImportFunc impFunc;
        impFunc.name = "get_current_ledger_obj_field";
        impFunc.gas = 70;

        static constexpr std::array<std::int32_t, 4> sizes = {
            32, 256, 1024, 4096};

        std::vector<std::pair<double, double>> sizeMedian;
        sizeMedian.reserve(sizes.size());

        log << "\n";
        log << "=== getCurrentLedgerObjField sweep -- HARDENED (real impl) "
               "===\n";
        log << "  Per-call defenses: asm volatile compiler barriers + "
               "cycling\n";
        log << "  out_ptr through " << kNumDests
            << " distinct destinations.\n";
        log << "  Compare against Test 2 to detect cross-iteration "
               "optimization.\n";
        log << "  Expected defense overhead in intercept: ~3-6 ns.\n";
        log << "\n";
        log << "  output bytes |  median ns | IQR/median%\n";
        log << "  -------------+------------+-------------\n";

        int idx = 0;
        for (auto const size : sizes)
        {
            auto const k = keylet::escrow(env.master, /*seq*/ 9800 + idx);
            auto sle = std::make_shared<SLE>(k);
            Blob payload(static_cast<std::size_t>(size), 0xAB);
            sle->setFieldVL(sfData, payload);
            ov.rawInsert(sle);

            WasmHostFunctionsImpl hfs(ac, k);
            hfs.setRT(&rt);
            WasmUserData udata{&hfs, impFunc};

            // Mutable params -- out_ptr will be cycled per call.
            wasm_val_t paramsData[3]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = sfData.getCode();
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = 0;  // cycled
            paramsData[2].kind = WASM_I32;
            paramsData[2].of.i32 = 8192;  // capacity

            wasm_val_vec_t params{};
            params.size = 3;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            // Sanity (offset 0).
            rt.resetGas(kGasBudgetPerSample);
            auto* trap =
                getCurrentLedgerObjField_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            BEAST_EXPECT(resultsData[0].of.i32 == size);

            // Hardened timed loop: cycle out_ptr per call + asm barriers.
            int callCount = 0;
            auto const samples = runTimedLoop(rt, [&]() {
                auto const out_ptr =
                    (callCount++ & (kNumDests - 1)) * kDestSpacing;
                paramsData[1].of.i32 = out_ptr;

                getCurrentLedgerObjField_wrap(&udata, &params, &results);

                doNotOptimize(rt.memData()[out_ptr]);
                doNotOptimize(resultsData[0].of.i32);
            });
            auto const stats = computeStats(samples);

            std::ostringstream row;
            row << "  " << std::setw(12) << size << " | " << std::setw(10)
                << stats.median << " | " << std::fixed
                << std::setprecision(2) << stats.iqrPct << "%";
            log << row.str() << "\n";

            sizeMedian.emplace_back(
                static_cast<double>(size),
                static_cast<double>(stats.median));
            ++idx;
        }

        auto const [slope, intercept] = linearFit(sizeMedian);
        log << "  linear fit: time(N) = " << intercept << " + " << slope
            << " * N (ns)\n";
        log << "  intercept ns:        " << intercept
            << "  (existing fixed gas: " << impFunc.gas << ")\n";
        log << "  slope ns/byte:       " << slope << "\n";
        constexpr int kSmall = 32;
        constexpr int kLimit = 4096;
        double const timeSmall =
            intercept + slope * static_cast<double>(kSmall);
        double const timeLimit =
            intercept + slope * static_cast<double>(kLimit);
        log << "  ns per gas unit (at N=" << kSmall
            << "): " << (timeSmall / static_cast<double>(impFunc.gas))
            << "\n";
        log << "  ns per gas unit (at N=" << kLimit
            << "): " << (timeLimit / static_cast<double>(impFunc.gas))
            << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2-differential: pair each call-based measurement with an
    // identical-loop no-call counterpart. Subtracting (with_call - without_call)
    // gives the pure wrapper-call cost, free of loop and harness overhead.
    //
    // If the differential slope matches the hardened-test slope, we have
    // strong (~99.9%) confidence the measured numbers reflect real work.
    // -----------------------------------------------------------------------
    void
    testGetCurrentLedgerObjFieldDifferential()
    {
        testcase(
            "getCurrentLedgerObjField sweep -- DIFFERENTIAL "
            "(call vs no-call)");

        using namespace jtx;
        Env env{*this};

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

        constexpr int kNumDests = 16;
        constexpr int kDestSpacing = 4096;
        BenchRuntime rt(/*memSize*/ kNumDests * kDestSpacing + 8 * 1024);

        WasmImportFunc impFunc;
        impFunc.name = "get_current_ledger_obj_field";
        impFunc.gas = 70;

        static constexpr std::array<std::int32_t, 4> sizes = {
            32, 256, 1024, 4096};

        std::vector<std::pair<double, double>> diffMedian;
        diffMedian.reserve(sizes.size());

        log << "\n";
        log << "=== getCurrentLedgerObjField sweep -- DIFFERENTIAL ===\n";
        log << "  with_call    = hardened-style measurement (cycles out_ptr,\n";
        log << "                 asm barriers, calls the wrapper).\n";
        log << "  without_call = identical loop, same param updates, same\n";
        log << "                 barriers, but wrapper call OMITTED.\n";
        log << "  diff         = pure wrapper call cost.\n";
        log << "\n";
        log << "  output bytes |  with_call ns | without_call ns | diff ns\n";
        log << "  -------------+---------------+-----------------+--------\n";

        int idx = 0;
        for (auto const size : sizes)
        {
            // Build SLE with sfData of size N.
            auto const k = keylet::escrow(env.master, /*seq*/ 9900 + idx);
            auto sle = std::make_shared<SLE>(k);
            Blob payload(static_cast<std::size_t>(size), 0xAB);
            sle->setFieldVL(sfData, payload);
            ov.rawInsert(sle);

            WasmHostFunctionsImpl hfs(ac, k);
            hfs.setRT(&rt);
            WasmUserData udata{&hfs, impFunc};

            wasm_val_t paramsData[3]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = sfData.getCode();
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = 0;  // cycled
            paramsData[2].kind = WASM_I32;
            paramsData[2].of.i32 = 8192;

            wasm_val_vec_t params{};
            params.size = 3;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            // Sanity (offset 0).
            rt.resetGas(kGasBudgetPerSample);
            auto* trap =
                getCurrentLedgerObjField_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            BEAST_EXPECT(resultsData[0].of.i32 == size);

            // --- Half A: with wrapper call ---
            int callA = 0;
            auto const samplesWith = runTimedLoop(rt, [&]() {
                auto const out_ptr =
                    (callA++ & (kNumDests - 1)) * kDestSpacing;
                paramsData[1].of.i32 = out_ptr;

                getCurrentLedgerObjField_wrap(&udata, &params, &results);

                doNotOptimize(rt.memData()[out_ptr]);
                doNotOptimize(resultsData[0].of.i32);
            });
            auto const statsWith = computeStats(samplesWith);

            // --- Half B: identical loop, no wrapper call ---
            int callB = 0;
            auto const samplesWithout = runTimedLoop(rt, [&]() {
                auto const out_ptr =
                    (callB++ & (kNumDests - 1)) * kDestSpacing;
                paramsData[1].of.i32 = out_ptr;

                // Wrapper call OMITTED. Everything else identical.

                doNotOptimize(rt.memData()[out_ptr]);
                doNotOptimize(resultsData[0].of.i32);
            });
            auto const statsWithout = computeStats(samplesWithout);

            std::int64_t const diff = statsWith.median - statsWithout.median;

            std::ostringstream row;
            row << "  " << std::setw(12) << size << " | " << std::setw(13)
                << statsWith.median << " | " << std::setw(15)
                << statsWithout.median << " | " << std::setw(7) << diff;
            log << row.str() << "\n";

            diffMedian.emplace_back(
                static_cast<double>(size), static_cast<double>(diff));
            ++idx;
        }

        auto const [slope, intercept] = linearFit(diffMedian);
        log << "  linear fit on diff: time(N) = " << intercept << " + "
            << slope << " * N (ns)\n";
        log << "  intercept (pure call) ns:  " << intercept << "\n";
        log << "  slope (pure per-byte) ns:  " << slope << "\n";
        constexpr int kSmall = 32;
        constexpr int kLimit = 4096;
        double const timeSmall =
            intercept + slope * static_cast<double>(kSmall);
        double const timeLimit =
            intercept + slope * static_cast<double>(kLimit);
        log << "  pure-call ns at N=" << kSmall << ": " << timeSmall << "\n";
        log << "  pure-call ns at N=" << kLimit << ": " << timeLimit << "\n";
        log << "  pure-call ns per gas (at N=" << kSmall
            << "): " << (timeSmall / static_cast<double>(impFunc.gas)) << "\n";
        log << "  pure-call ns per gas (at N=" << kLimit
            << "): " << (timeLimit / static_cast<double>(impFunc.gas)) << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2b: getCurrentLedgerObjField returning a UInt32 field.
    //
    // Same wrapper as Test 2, but the SLE field is a fixed 4-byte uint32
    // instead of a variable-length Blob. The impl path goes through
    // getAnyFieldData on a non-VL field type, which serializes the integer
    // to bytes (no host-side allocation of an externally-owned buffer the
    // way Blob does). Comparing this measurement to Test 2's smallest row
    // tells us how much of the get-field cost depends on the field type.
    // -----------------------------------------------------------------------
    void
    testGetCurrentLedgerObjFieldInt()
    {
        testcase("getCurrentLedgerObjField on uint32 field (real impl)");

        using namespace jtx;
        Env env{*this};

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

        // Build a synthetic escrow SLE with a uint32 field set.
        auto const k = keylet::escrow(env.master, /*seq*/ 9500);
        auto sle = std::make_shared<SLE>(k);
        sle->setFieldU32(sfPreviousTxnLgrSeq, 0x12345678);
        ov.rawInsert(sle);

        WasmHostFunctionsImpl hfs(ac, k);

        // 64 bytes is plenty for a 4-byte uint32 result.
        BenchRuntime rt(/*memSize*/ 64);
        hfs.setRT(&rt);

        WasmImportFunc impFunc;
        impFunc.name = "get_current_ledger_obj_field";
        impFunc.gas = 70;
        WasmUserData udata{&hfs, impFunc};

        // params: (sfield_code, out_ptr, out_len)
        wasm_val_t paramsData[3]{};
        paramsData[0].kind = WASM_I32;
        paramsData[0].of.i32 = sfPreviousTxnLgrSeq.getCode();
        paramsData[1].kind = WASM_I32;
        paramsData[1].of.i32 = 0;  // out_ptr
        paramsData[2].kind = WASM_I32;
        paramsData[2].of.i32 = 32;  // out_len capacity (4 bytes will be written)

        wasm_val_vec_t params{};
        params.size = 3;
        params.data = paramsData;

        wasm_val_t resultsData[1]{};
        resultsData[0].kind = WASM_I32;
        wasm_val_vec_t results{};
        results.size = 1;
        results.data = resultsData;

        rt.resetGas(kGasBudgetPerSample);
        auto* trap = getCurrentLedgerObjField_wrap(&udata, &params, &results);
        BEAST_EXPECT(trap == nullptr);
        BEAST_EXPECT(resultsData[0].of.i32 == 4);  // uint32 -> 4 bytes

        auto const samples = runTimedLoop(rt, [&]() {
            getCurrentLedgerObjField_wrap(&udata, &params, &results);
            benchSink_ ^= rt.memData()[0];
            benchSink_ ^= static_cast<std::uint64_t>(resultsData[0].of.i32);
        });
        auto const stats = computeStats(samples);

        log << "\n";
        log << "=== getCurrentLedgerObjField on uint32 field (real impl) "
               "===\n";
        log << "  samples:          " << kSamples << " (each = " << kInner
            << " calls amortized)\n";
        log << "  median ns/call:   " << stats.median << "\n";
        log << "  p25 / p75 ns:     " << stats.p25 << " / " << stats.p75
            << "\n";
        log << "  IQR / median:     " << std::fixed << std::setprecision(2)
            << stats.iqrPct << "%\n";
        log << "  per-call gas:     " << impFunc.gas << "\n";
        log << "  ns per gas unit:  "
            << (static_cast<double>(stats.median) /
                static_cast<double>(impFunc.gas))
            << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2c: getCurrentLedgerObjField returning a Hash256 field.
    //
    // Fixed-size 32-byte field (sfPreviousTxnID, present on every SLE),
    // third data point alongside variable-blob (Test 2) and uint32 (Test
    // 2b). Lets us see how the per-call cost varies across SField types
    // of different sizes / serialization paths.
    // -----------------------------------------------------------------------
    void
    testGetCurrentLedgerObjFieldHash()
    {
        testcase("getCurrentLedgerObjField on Hash256 field (real impl)");

        using namespace jtx;
        Env env{*this};

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

        auto const k = keylet::escrow(env.master, /*seq*/ 9501);
        auto sle = std::make_shared<SLE>(k);
        sle->setFieldH256(sfPreviousTxnID, uint256{});
        ov.rawInsert(sle);

        WasmHostFunctionsImpl hfs(ac, k);

        BenchRuntime rt(/*memSize*/ 64);
        hfs.setRT(&rt);

        WasmImportFunc impFunc;
        impFunc.name = "get_current_ledger_obj_field";
        impFunc.gas = 70;
        WasmUserData udata{&hfs, impFunc};

        // params: (sfield_code, out_ptr, out_len)
        wasm_val_t paramsData[3]{};
        paramsData[0].kind = WASM_I32;
        paramsData[0].of.i32 = sfPreviousTxnID.getCode();
        paramsData[1].kind = WASM_I32;
        paramsData[1].of.i32 = 0;
        paramsData[2].kind = WASM_I32;
        paramsData[2].of.i32 = 32;

        wasm_val_vec_t params{};
        params.size = 3;
        params.data = paramsData;

        wasm_val_t resultsData[1]{};
        resultsData[0].kind = WASM_I32;
        wasm_val_vec_t results{};
        results.size = 1;
        results.data = resultsData;

        rt.resetGas(kGasBudgetPerSample);
        auto* trap = getCurrentLedgerObjField_wrap(&udata, &params, &results);
        BEAST_EXPECT(trap == nullptr);
        BEAST_EXPECT(resultsData[0].of.i32 == 32);  // Hash256 = 32 bytes

        auto const samples = runTimedLoop(rt, [&]() {
            getCurrentLedgerObjField_wrap(&udata, &params, &results);
            benchSink_ ^= rt.memData()[0];
            benchSink_ ^= static_cast<std::uint64_t>(resultsData[0].of.i32);
        });
        auto const stats = computeStats(samples);

        log << "\n";
        log << "=== getCurrentLedgerObjField on Hash256 field (real impl) "
               "===\n";
        log << "  samples:          " << kSamples << " (each = " << kInner
            << " calls amortized)\n";
        log << "  median ns/call:   " << stats.median << "\n";
        log << "  p25 / p75 ns:     " << stats.p25 << " / " << stats.p75
            << "\n";
        log << "  IQR / median:     " << std::fixed << std::setprecision(2)
            << stats.iqrPct << "%\n";
        log << "  per-call gas:     " << impFunc.gas << "\n";
        log << "  ns per gas unit:  "
            << (static_cast<double>(stats.median) /
                static_cast<double>(impFunc.gas))
            << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2d: cacheLedgerObj sweep over SLE size.
    //
    // FLOOR MEASUREMENT ONLY. Tests use the in-memory NodeStore, so this
    // captures the steady-state cost (OpenView cache hit + cache_ slot
    // overwrite), NOT the cold disk-read + SHAMap-deserialize cost that
    // dominates real production calls. Real cost on a slow validator with
    // a cold NuDB cache can be 10-1000x slower than what this test reports.
    //
    // The slope still tells us how much per-byte work happens in the
    // cached path (likely near zero, since OpenView returns a cached
    // shared_ptr without re-deserializing).
    // -----------------------------------------------------------------------
    void
    testCacheLedgerObjSweep()
    {
        testcase(
            "cacheLedgerObj sweep (real impl, in-memory ledger -- FLOOR)");

        using namespace jtx;
        Env env{*this};

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

        BenchRuntime rt(/*memSize*/ 256);

        WasmImportFunc impFunc;
        impFunc.name = "cache_ledger_obj";
        impFunc.gas = 5000;

        constexpr std::int32_t kKeyletOffset = 0;
        constexpr std::int32_t kKeyletSize = 32;  // uint256

        // Sweep over the SLE's sfData payload size; the wrapper does not
        // care about size, but if any SLE-size-dependent work happens in
        // the cached path it'll show up as a slope.
        static constexpr std::array<std::int32_t, 5> sizes = {
            32, 128, 512, 1024, 4096};

        std::vector<std::pair<double, double>> sizeMedian;
        sizeMedian.reserve(sizes.size());

        log << "\n";
        log << "=== cacheLedgerObj sweep (real impl, IN-MEMORY -- FLOOR) "
               "===\n";
        log << "  Tests use in-memory NodeStore. Steady-state cache-hit\n";
        log << "  cost only; real production cold-disk reads can be\n";
        log << "  10-1000x slower than this measurement.\n";
        log << "\n";
        log << "  SLE sfData bytes |  median ns | IQR/median%\n";
        log << "  -----------------+------------+-------------\n";

        int idx = 0;
        for (auto const size : sizes)
        {
            // Build a synthetic escrow SLE with sfData of size N.
            auto const k =
                keylet::escrow(env.master, /*seq*/ 9700 + idx);
            auto sle = std::make_shared<SLE>(k);
            Blob payload(static_cast<std::size_t>(size), 0xAB);
            sle->setFieldVL(sfData, payload);
            ov.rawInsert(sle);

            // Place the keylet's 32-byte key into wasm memory at
            // kKeyletOffset so getDataUInt256 in the wrapper reads it.
            std::memcpy(
                rt.memData() + kKeyletOffset,
                k.key.data(),
                static_cast<std::size_t>(kKeyletSize));

            // Fresh hfs (cache_ slots empty).
            WasmHostFunctionsImpl hfs(ac, k);
            hfs.setRT(&rt);
            WasmUserData udata{&hfs, impFunc};

            // params: (objId_ptr, objId_len, cacheIdx).
            // cacheIdx=1 maps to slot 0 after normalization, so each call
            // overwrites slot 0 (avoids filling the cache_ array).
            wasm_val_t paramsData[3]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = kKeyletOffset;
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = kKeyletSize;
            paramsData[2].kind = WASM_I32;
            paramsData[2].of.i32 = 1;

            wasm_val_vec_t params{};
            params.size = 3;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            auto* trap = cacheLedgerObj_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            BEAST_EXPECT(resultsData[0].of.i32 == 1);  // returned slot

            auto const samples = runTimedLoop(rt, [&]() {
                cacheLedgerObj_wrap(&udata, &params, &results);
                benchSink_ ^=
                    static_cast<std::uint64_t>(resultsData[0].of.i32);
            });
            auto const stats = computeStats(samples);

            std::ostringstream row;
            row << "  " << std::setw(16) << size << " | " << std::setw(10)
                << stats.median << " | " << std::fixed
                << std::setprecision(2) << stats.iqrPct << "%";
            log << row.str() << "\n";

            sizeMedian.emplace_back(
                static_cast<double>(size),
                static_cast<double>(stats.median));
            ++idx;
        }

        auto const [slope, intercept] = linearFit(sizeMedian);
        log << "  linear fit: time(N) = " << intercept << " + " << slope
            << " * N (ns)\n";
        log << "  intercept ns:        " << intercept
            << "  (existing fixed gas: " << impFunc.gas << ")\n";
        log << "  slope ns/byte:       " << slope << "\n";
        constexpr int kSmall = 32;
        constexpr int kBig = 4096;
        double const timeSmall = intercept + slope * static_cast<double>(kSmall);
        double const timeBig = intercept + slope * static_cast<double>(kBig);
        log << "  ns per gas unit (at N=" << kSmall
            << ", FLOOR): " << (timeSmall / static_cast<double>(impFunc.gas))
            << "\n";
        log << "  ns per gas unit (at N=" << kBig
            << ", FLOOR): " << (timeBig / static_cast<double>(impFunc.gas))
            << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 2e: updateData sweep over input size.
    //
    // FLOOR MEASUREMENT ONLY. updateData stages the input into the impl's
    // data_ field (alloc + memcpy). The REAL ledger write at apply time
    // (SHAMap update + NuDB write + eventually disk) happens AFTER WASM
    // execution and is NOT measured here.
    //
    // What this test captures: the per-call cost of one heap allocation
    // plus one byte-proportional memcpy on the input slice. That's the
    // wrapper-layer floor for `updateData`.
    // -----------------------------------------------------------------------
    void
    testUpdateDataSweep()
    {
        testcase(
            "updateData sweep (real impl, host-side staging ONLY -- FLOOR)");

        using namespace jtx;
        Env env{*this};

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

        auto const dummyEscrow =
            keylet::escrow(env.master, env.seq(env.master));
        WasmHostFunctionsImpl hfs(ac, dummyEscrow);

        // 8 KB: room for max 4 KB input + headroom.
        BenchRuntime rt(/*memSize*/ 8 * 1024);
        hfs.setRT(&rt);

        // Fill the input region with a deterministic non-zero pattern.
        for (std::size_t i = 0; i < 8u * 1024u; ++i)
            rt.memData()[i] = static_cast<std::uint8_t>(i & 0xff);

        WasmImportFunc impFunc;
        impFunc.name = "update_data";
        impFunc.gas = 1000;
        WasmUserData udata{&hfs, impFunc};

        // updateData uses isUpdate=true in getDataSlice, so the limit is
        // maxWasmDataLength = 4096 (vs maxWasmParamLength = 1024 for
        // most other slice-taking wrappers).
        static constexpr std::array<std::int32_t, 5> sizes = {
            32, 128, 512, 1024, 4096};

        std::vector<std::pair<double, double>> sizeMedian;
        sizeMedian.reserve(sizes.size());

        log << "\n";
        log << "=== updateData sweep (real impl, host-side STAGING -- "
               "FLOOR) ===\n";
        log << "  Measures only the host-side alloc + memcpy of the input\n";
        log << "  into hfs.data_. The ledger write at apply time is NOT\n";
        log << "  measured -- it happens after WASM execution.\n";
        log << "\n";
        log << "  input bytes |  median ns | IQR/median%\n";
        log << "  ------------+------------+-------------\n";

        for (auto const size : sizes)
        {
            // params: (in_ptr, in_len)
            wasm_val_t paramsData[2]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = 0;
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = size;

            wasm_val_vec_t params{};
            params.size = 2;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            auto* trap = updateData_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            BEAST_EXPECT(resultsData[0].of.i32 == size);

            auto const samples = runTimedLoop(rt, [&]() {
                updateData_wrap(&udata, &params, &results);
                benchSink_ ^= rt.memData()[0];
                benchSink_ ^=
                    static_cast<std::uint64_t>(resultsData[0].of.i32);
            });
            auto const stats = computeStats(samples);

            std::ostringstream row;
            row << "  " << std::setw(11) << size << " | " << std::setw(10)
                << stats.median << " | " << std::fixed
                << std::setprecision(2) << stats.iqrPct << "%";
            log << row.str() << "\n";

            sizeMedian.emplace_back(
                static_cast<double>(size),
                static_cast<double>(stats.median));
        }

        auto const [slope, intercept] = linearFit(sizeMedian);
        log << "  linear fit: time(N) = " << intercept << " + " << slope
            << " * N (ns)\n";
        log << "  intercept ns:        " << intercept
            << "  (existing fixed gas: " << impFunc.gas << ")\n";
        log << "  slope ns/byte:       " << slope << "\n";
        constexpr int kSmall = 32;
        constexpr int kLimit = 4096;
        double const timeSmall = intercept + slope * static_cast<double>(kSmall);
        double const timeLimit = intercept + slope * static_cast<double>(kLimit);
        log << "  ns per gas unit (at N=" << kSmall
            << ", FLOOR): " << (timeSmall / static_cast<double>(impFunc.gas))
            << "\n";
        log << "  ns per gas unit (at N=" << kLimit
            << ", FLOOR): " << (timeLimit / static_cast<double>(impFunc.gas))
            << "\n";
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 3: computeSha512HalfHash sweep over input size (real impl).
    // -----------------------------------------------------------------------
    void
    testComputeSha512HashSweep()
    {
        testcase("computeSha512HalfHash sweep (real impl)");

        using namespace jtx;
        Env env{*this};

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

        // Generous: well over the 1KB wasm limit + 32-byte hash output.
        BenchRuntime rt(/*memSize*/ 8 * 1024);
        hfs.setRT(&rt);

        WasmImportFunc impFunc;
        impFunc.name = "compute_sha512_half";
        impFunc.gas = 2000;
        WasmUserData udata{&hfs, impFunc};

        constexpr std::int32_t kInputOffset = 0;
        constexpr std::int32_t kOutputOffset = 4096;
        constexpr std::int32_t kHashSize = 32;

        // Fill input region with a non-zero deterministic pattern.
        auto* memBytes = rt.memData();
        for (std::size_t i = 0; i < 8u * 1024u; ++i)
            memBytes[i] = static_cast<std::uint8_t>(i & 0xff);

        // In-bounds sweep (used for the linear fit). maxWasmParamLength = 1024.
        static constexpr std::array<std::int32_t, 4> sizes = {
            32, 128, 512, 1024};
        // Single over-limit row: triggers the wrapper's getDataSlice rejection
        // path (DATA_FIELD_TOO_LARGE). Reported but excluded from the fit.
        constexpr std::int32_t kOversize = 2048;

        std::vector<std::pair<double, double>> sizeMedian;
        sizeMedian.reserve(sizes.size());

        log << "\n";
        log << "=== computeSha512HalfHash sweep (real impl) ===\n";
        log << "  input bytes |  median ns | IQR/median%\n";
        log << "  ------------+------------+-------------\n";

        auto runOne = [&](std::int32_t size, bool expectSuccess) -> Stats {
            wasm_val_t paramsData[4]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = kInputOffset;
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = size;
            paramsData[2].kind = WASM_I32;
            paramsData[2].of.i32 = kOutputOffset;
            paramsData[3].kind = WASM_I32;
            paramsData[3].of.i32 = kHashSize;

            wasm_val_vec_t params{};
            params.size = 4;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            auto* trap = computeSha512HalfHash_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            (void)expectSuccess;

            auto const samples = runTimedLoop(rt, [&]() {
                computeSha512HalfHash_wrap(&udata, &params, &results);
                // Sink: read a hash output byte and the result code so the
                // hashing work is observable each iteration.
                benchSink_ ^= rt.memData()[kOutputOffset];
                benchSink_ ^=
                    static_cast<std::uint64_t>(resultsData[0].of.i32);
            });
            return computeStats(samples);
        };

        for (auto const size : sizes)
        {
            auto const stats = runOne(size, /*expectSuccess*/ true);
            std::ostringstream row;
            row << "  " << std::setw(11) << size << " | " << std::setw(10)
                << stats.median << " | " << std::fixed << std::setprecision(2)
                << stats.iqrPct << "%";
            log << row.str() << "\n";
            sizeMedian.emplace_back(
                static_cast<double>(size),
                static_cast<double>(stats.median));
        }

        // Over-limit row (rejection path). Reported, not fitted.
        {
            auto const stats = runOne(kOversize, /*expectSuccess*/ false);
            std::ostringstream row;
            row << "  " << std::setw(11) << kOversize << " | " << std::setw(10)
                << stats.median << " | " << std::fixed << std::setprecision(2)
                << stats.iqrPct << "%   [over limit, rejection path]";
            log << row.str() << "\n";
        }

        auto const [slope, intercept] = linearFit(sizeMedian);
        log << "  linear fit: time(N) = " << intercept << " + " << slope
            << " * N (ns)\n";
        log << "  intercept ns:        " << intercept
            << "  (existing fixed gas: " << impFunc.gas << ")\n";
        log << "  slope ns/byte:       " << slope << "\n";
        // "ns per gas unit" at max valid input (maxWasmParamLength) and at
        // the smallest swept size.
        constexpr int kSmall = 32;
        constexpr int kLimit = 1024;  // maxWasmParamLength
        double const timeAtSmall = intercept + slope * static_cast<double>(kSmall);
        double const timeAtLimit = intercept + slope * static_cast<double>(kLimit);
        log << "  ns per gas unit (at N=" << kSmall
            << "): " << (timeAtSmall / static_cast<double>(impFunc.gas)) << "\n";
        log << "  ns per gas unit (at N=" << kLimit
            << "): " << (timeAtLimit / static_cast<double>(impFunc.gas)) << "\n";
        if (nsPerGas_ > 0)
        {
            log << "  intercept as gas:    " << (intercept / nsPerGas_)
                << "  (using baseline calibration "
                << nsPerGas_ << " ns/gas)\n";
            log << "  slope as gas/byte:   " << (slope / nsPerGas_) << "\n";
        }
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 4: checkSignature sweep over message size (real impl).
    // -----------------------------------------------------------------------
    void
    testCheckSignatureSweep()
    {
        testcase("checkSignature sweep (real impl)");

        using namespace jtx;
        Env env{*this};

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

        // 8 KB: max valid msg 1KB + over-limit 2KB + sig (~70B) + key (~33B),
        // with comfortable padding.
        BenchRuntime rt(/*memSize*/ 8 * 1024);
        hfs.setRT(&rt);

        WasmImportFunc impFunc;
        impFunc.name = "check_sig";
        impFunc.gas = 35000;
        WasmUserData udata{&hfs, impFunc};

        // Pre-generate a deterministic key pair.
        auto const seed = randomSeed();
        auto const [pubKey, secKey] =
            generateKeyPair(KeyType::secp256k1, seed);

        constexpr std::int32_t kMsgOffset = 0;
        constexpr std::int32_t kSigGoodOffset = 4096;
        constexpr std::int32_t kSigBadOffset = 4400;
        constexpr std::int32_t kKeyOffset = 4700;

        auto* memBytes = rt.memData();

        // Fill message region with a deterministic pattern.
        for (std::size_t i = 0; i < 4096u; ++i)
            memBytes[i] = static_cast<std::uint8_t>(i & 0xff);

        // Place the public key once (reused for every size).
        std::memcpy(memBytes + kKeyOffset, pubKey.data(), pubKey.size());
        auto const pubKeySize = static_cast<std::int32_t>(pubKey.size());

        static constexpr std::array<std::int32_t, 4> sizes = {
            32, 128, 512, 1024};
        constexpr std::int32_t kOversize = 2048;

        std::vector<std::pair<double, double>> sizeMedianMax;
        sizeMedianMax.reserve(sizes.size());

        log << "\n";
        log << "=== checkSignature sweep (real impl, secp256k1) ===\n";
        log << "  Two columns per size:\n";
        log << "    good = signature valid for the message (returns 1)\n";
        log << "    bad  = signature valid for a *different* message of same\n";
        log << "           size -- well-formed, full verify path, returns 0.\n";
        log << "  Linear fit uses max(good, bad) per size (worst case for\n";
        log << "  gas charging).\n";
        log << "\n";
        log << "  msg bytes   |  good ns | good IQR% |  bad ns  | bad IQR%\n";
        log << "  ------------+----------+-----------+----------+----------\n";

        // Pre-build params + results vectors; mutate ptrs/lens per iteration.
        wasm_val_t paramsData[6]{};
        for (auto& p : paramsData)
            p.kind = WASM_I32;
        paramsData[0].of.i32 = kMsgOffset;
        paramsData[4].of.i32 = kKeyOffset;
        paramsData[5].of.i32 = pubKeySize;

        wasm_val_vec_t params{};
        params.size = 6;
        params.data = paramsData;

        wasm_val_t resultsData[1]{};
        resultsData[0].kind = WASM_I32;
        wasm_val_vec_t results{};
        results.size = 1;
        results.data = resultsData;

        // Sig verify is ~200-300 microseconds per call in debug -- already
        // ~10,000x clock granularity. No inner amortization needed; far fewer
        // samples are statistically sufficient.
        constexpr int kSigInner = 10;
        constexpr int kSigSamples = 500;
        constexpr int kSigWarmup = 5;

        // Run one timed loop for a given (msgLen, sigOffset, sigLen). If
        // expectedResult >= 0, sanity-check the result code; pass -1 to skip
        // (e.g. for the over-limit case where the rejection error code is
        // returned, not 0/1).
        auto timeOnce = [&](std::int32_t msgLen,
                            std::int32_t sigOffset,
                            std::int32_t sigLen,
                            int expectedResult) -> Stats {
            paramsData[1].of.i32 = msgLen;
            paramsData[2].of.i32 = sigOffset;
            paramsData[3].of.i32 = sigLen;

            rt.resetGas(kGasBudgetPerSample);
            auto* trap = checkSignature_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            if (expectedResult >= 0)
                BEAST_EXPECT(resultsData[0].of.i32 == expectedResult);

            auto const samples = runTimedLoop(
                rt,
                [&]() {
                    checkSignature_wrap(&udata, &params, &results);
                    // Sink: result code (0 or 1) is observable each
                    // iteration so the verify isn't elided.
                    benchSink_ ^=
                        static_cast<std::uint64_t>(resultsData[0].of.i32);
                },
                kSigInner,
                kSigSamples,
                kSigWarmup);
            return computeStats(samples);
        };

        for (auto const size : sizes)
        {
            // good = sig over the actual message bytes.
            Slice const msg(
                memBytes + kMsgOffset, static_cast<std::size_t>(size));
            Buffer const goodSig = sign(pubKey, secKey, msg);
            std::memcpy(
                memBytes + kSigGoodOffset, goodSig.data(), goodSig.size());

            // bad = sig over a *different* message of the same size. Valid
            // DER, valid r/s; verify runs the full curve op and returns 0.
            std::vector<std::uint8_t> altMsg(static_cast<std::size_t>(size));
            for (std::int32_t i = 0; i < size; ++i)
                altMsg[static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>(0xff - (i & 0xff));
            Slice const altSlice(altMsg.data(), altMsg.size());
            Buffer const badSig = sign(pubKey, secKey, altSlice);
            std::memcpy(
                memBytes + kSigBadOffset, badSig.data(), badSig.size());

            auto const goodStats = timeOnce(
                size,
                kSigGoodOffset,
                static_cast<std::int32_t>(goodSig.size()),
                /*expectedResult*/ 1);
            auto const badStats = timeOnce(
                size,
                kSigBadOffset,
                static_cast<std::int32_t>(badSig.size()),
                /*expectedResult*/ 0);

            std::ostringstream row;
            row << "  " << std::setw(11) << size << " | " << std::setw(8)
                << goodStats.median << " | " << std::fixed
                << std::setprecision(2) << std::setw(8) << goodStats.iqrPct
                << "% | " << std::setw(8) << badStats.median << " | "
                << std::setw(7) << badStats.iqrPct << "%";
            log << row.str() << "\n";

            // Linear fit uses the more expensive of the two cases.
            sizeMedianMax.emplace_back(
                static_cast<double>(size),
                static_cast<double>(
                    std::max(goodStats.median, badStats.median)));
        }

        // Over-limit row: getDataSlice rejects the message before sig is
        // examined, so the good/bad sig content does not matter; both columns
        // measure the same rejection-path cost. Reused placeholder signature
        // (signed over a 32-byte msg) is valid format -- the wrapper never
        // looks at it.
        {
            Slice const tinyMsg(memBytes + kMsgOffset, 32);
            Buffer const placeholder = sign(pubKey, secKey, tinyMsg);
            std::memcpy(
                memBytes + kSigGoodOffset,
                placeholder.data(),
                placeholder.size());
            std::memcpy(
                memBytes + kSigBadOffset,
                placeholder.data(),
                placeholder.size());
            auto const sigSz = static_cast<std::int32_t>(placeholder.size());

            auto const goodStats =
                timeOnce(kOversize, kSigGoodOffset, sigSz, -1);
            auto const badStats =
                timeOnce(kOversize, kSigBadOffset, sigSz, -1);

            std::ostringstream row;
            row << "  " << std::setw(11) << kOversize << " | " << std::setw(8)
                << goodStats.median << " | " << std::fixed
                << std::setprecision(2) << std::setw(8) << goodStats.iqrPct
                << "% | " << std::setw(8) << badStats.median << " | "
                << std::setw(7) << badStats.iqrPct
                << "%   [over limit, rejection path]";
            log << row.str() << "\n";
        }

        auto const [slope, intercept] = linearFit(sizeMedianMax);
        log << "  linear fit on max(good,bad): time(N) = " << intercept
            << " + " << slope << " * N (ns)\n";
        log << "  intercept ns:        " << intercept
            << "  (existing fixed gas: " << impFunc.gas << ")\n";
        log << "  slope ns/byte:       " << slope << "\n";
        // "ns per gas unit" at max valid message size and at the smallest
        // swept size.
        constexpr int kSmall = 32;
        constexpr int kLimit = 1024;  // maxWasmParamLength
        double const timeAtSmall = intercept + slope * static_cast<double>(kSmall);
        double const timeAtLimit = intercept + slope * static_cast<double>(kLimit);
        log << "  ns per gas unit (at N=" << kSmall
            << "): " << (timeAtSmall / static_cast<double>(impFunc.gas)) << "\n";
        log << "  ns per gas unit (at N=" << kLimit
            << "): " << (timeAtLimit / static_cast<double>(impFunc.gas)) << "\n";
        if (nsPerGas_ > 0)
        {
            log << "  intercept as gas:    " << (intercept / nsPerGas_)
                << "  (using baseline calibration " << nsPerGas_
                << " ns/gas)\n";
            log << "  slope as gas/byte:   " << (slope / nsPerGas_) << "\n";
        }
        log << std::endl;
    }

    // -----------------------------------------------------------------------
    // Test 5: single-call debug stepping.
    //
    // Invokes each measured wrapper exactly once with the same context
    // construction the benchmark uses. No timing loop, no inner amortization.
    // Set a breakpoint on the marked lines and step into the wrapper to
    // trace exactly what happens (chargeCall -> getDataXxx -> hf->...
    // -> chargeMem/chargeCompute -> returnResult -> setData -> hfResult).
    // -----------------------------------------------------------------------
    void
    testSingleCallStep()
    {
        testcase("Single-call debug stepping");

        using namespace jtx;
        Env env{*this};

        OpenView ov{*env.current()};
        STTx const tx(ttESCROW_FINISH, [](STObject&) {});
        ApplyContext ac{
            env.app(), ov, tx, tesSUCCESS, env.current()->fees().base, tapNONE, env.journal};

        // 16 KB: room for in-bounds inputs/outputs of all four wrappers.
        BenchRuntime rt(/*memSize*/ 16 * 1024);

        // Representative size for sweep tests.
        constexpr std::int32_t kSize = 1024;

        // Fill input region with deterministic non-zero pattern.
        for (std::size_t i = 0; i < 16u * 1024u; ++i)
            rt.memData()[i] = static_cast<std::uint8_t>(i & 0xff);
#if 0
        // ============================================================
        // (1) getLedgerSqn -- baseline call.
        // ============================================================
        {
            auto const dummyEscrow = keylet::escrow(env.master, env.seq(env.master));
            WasmHostFunctionsImpl hfs(ac, dummyEscrow);
            hfs.setRT(&rt);

            WasmImportFunc impFunc;
            impFunc.name = "get_ledger_sqn";
            impFunc.gas = 60;
            WasmUserData udata{&hfs, impFunc};

            wasm_val_t paramsData[2]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = 0;
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = 4;
            wasm_val_vec_t params{};
            params.size = 2;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            // <<< STEP-INTO HERE: getLedgerSqn_wrap >>>
            auto* trap = getLedgerSqn_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
        }
#endif
        // ============================================================
        // (2) getCurrentLedgerObjField -- chargeMem path.
        //     Synthetic escrow SLE with sfData = kSize bytes.
        // ============================================================
        {
            auto const k = keylet::escrow(env.master, /*seq*/ 9001);
            auto sle = std::make_shared<SLE>(k);
            Blob payload(static_cast<std::size_t>(kSize), 0xAB);
            sle->setFieldVL(sfData, payload);
            ov.rawInsert(sle);

            WasmHostFunctionsImpl hfs(ac, k);
            hfs.setRT(&rt);

            WasmImportFunc impFunc;
            impFunc.name = "get_current_ledger_obj_field";
            impFunc.gas = 70;
            WasmUserData udata{&hfs, impFunc};

            wasm_val_t paramsData[3]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = sfData.getCode();
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = 4096;  // out_ptr
            paramsData[2].kind = WASM_I32;
            paramsData[2].of.i32 = 8192;  // out_len capacity
            wasm_val_vec_t params{};
            params.size = 3;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            // <<< STEP-INTO HERE: getCurrentLedgerObjField_wrap >>>
            auto* trap = getCurrentLedgerObjField_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
        }
#if 0
        // ============================================================
        // (3) computeSha512HalfHash -- chargeCompute path.
        // ============================================================
        {
            auto const dummyEscrow = keylet::escrow(env.master, env.seq(env.master));
            WasmHostFunctionsImpl hfs(ac, dummyEscrow);
            hfs.setRT(&rt);

            WasmImportFunc impFunc;
            impFunc.name = "compute_sha512_half";
            impFunc.gas = 1500;
            WasmUserData udata{&hfs, impFunc};

            wasm_val_t paramsData[4]{};
            paramsData[0].kind = WASM_I32;
            paramsData[0].of.i32 = 0;  // in_ptr
            paramsData[1].kind = WASM_I32;
            paramsData[1].of.i32 = kSize;  // in_len
            paramsData[2].kind = WASM_I32;
            paramsData[2].of.i32 = 4096;  // out_ptr
            paramsData[3].kind = WASM_I32;
            paramsData[3].of.i32 = 32;  // out_len = hash size
            wasm_val_vec_t params{};
            params.size = 4;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            // <<< STEP-INTO HERE: computeSha512HalfHash_wrap >>>
            auto* trap = computeSha512HalfHash_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
        }

        // ============================================================
        // (4) checkSignature -- sig verify path. Pre-generate key + sig.
        // ============================================================
        {
            auto const dummyEscrow = keylet::escrow(env.master, env.seq(env.master));
            WasmHostFunctionsImpl hfs(ac, dummyEscrow);
            hfs.setRT(&rt);

            WasmImportFunc impFunc;
            impFunc.name = "check_sig";
            impFunc.gas = 35000;
            WasmUserData udata{&hfs, impFunc};

            auto const seed = randomSeed();
            auto const [pubKey, secKey] = generateKeyPair(KeyType::secp256k1, seed);

            constexpr std::int32_t kMsgOff = 0;
            constexpr std::int32_t kSigOff = 4096;
            constexpr std::int32_t kKeyOff = 4400;

            Slice const msg(rt.memData() + kMsgOff, kSize);
            Buffer const sig = sign(pubKey, secKey, msg);
            std::memcpy(rt.memData() + kSigOff, sig.data(), sig.size());
            std::memcpy(rt.memData() + kKeyOff, pubKey.data(), pubKey.size());

            wasm_val_t paramsData[6]{};
            for (auto& p : paramsData)
                p.kind = WASM_I32;
            paramsData[0].of.i32 = kMsgOff;
            paramsData[1].of.i32 = kSize;
            paramsData[2].of.i32 = kSigOff;
            paramsData[3].of.i32 = static_cast<std::int32_t>(sig.size());
            paramsData[4].of.i32 = kKeyOff;
            paramsData[5].of.i32 = static_cast<std::int32_t>(pubKey.size());
            wasm_val_vec_t params{};
            params.size = 6;
            params.data = paramsData;

            wasm_val_t resultsData[1]{};
            resultsData[0].kind = WASM_I32;
            wasm_val_vec_t results{};
            results.size = 1;
            results.data = resultsData;

            rt.resetGas(kGasBudgetPerSample);
            // <<< STEP-INTO HERE: checkSignature_wrap >>>
            auto* trap = checkSignature_wrap(&udata, &params, &results);
            BEAST_EXPECT(trap == nullptr);
            BEAST_EXPECT(resultsData[0].of.i32 == 1);
        }
#endif
    }

    // -----------------------------------------------------------------------
    // WASM-instruction anchor (real wasmi engine).
    //
    // Unlike the other tests in this suite, this set runs through
    // WasmEngine::instance().run() to measure ns-per-gas in the
    // interpreted-WASM regime (using wasmi's internal fuel metering).
    // The per-host-fn anchor from testGetLedgerSqnBaseline measures a
    // different quantity entirely: wrapper-internal native execution at
    // whatever gas the schedule assigns to that wrapper.
    //
    // We run six kernel shapes through the same harness to bracket the
    // realistic ns/gas range:
    //
    //   1. mixed-opcode   -- baseline; arith + locals + one fixed store/load.
    //                        Defines the i-cache-friendly floor.
    //   2. memory-scatter -- LCG-driven scattered loads/stores across 128 KB.
    //                        Defeats d-cache pinning of a single hot address.
    //   3. branch-heavy   -- two data-dependent unpredictable branches per
    //                        iteration. Defeats branch predictor learning.
    //   4. diverse-opcode -- ~17 distinct opcodes/iter including div_u/rem_u,
    //                        select, scattered memory, a function call, and
    //                        a data-dependent branch. Closest single kernel
    //                        to realistic mixed escrow shape; stresses
    //                        wasmi handler diversity (more native handlers
    //                        competing for L1 i-cache).
    //   5. call_indirect  -- 8-way table dispatch on a random index; tests
    //                        wasmi's table-bounds-check + runtime-type-check
    //                        + indirect-dispatch path. This is what Rust
    //                        trait objects and C function pointers compile
    //                        to and is the heaviest call shape in MVP.
    //   6. combined-worst -- call_indirect through a 4-way table of
    //                        4-parameter functions whose bodies contain
    //                        div_u/rem_u. Tests whether the expensive-op
    //                        costs we identified separately (indirect
    //                        dispatch + division + parameter marshaling)
    //                        compound when stacked into one per-iter call.
    //
    // The worst (highest) ns/gas across the six is the conservative
    // anchor for the interpreted regime.
    //
    // Differential trick (from volatile.md): time two iteration counts and
    // subtract. The differential cancels module instantiation, import
    // binding, and linear-memory setup -- isolating per-iteration
    // wall-clock and per-iteration gas. Their ratio is ns/gas.
    //
    // Note: even the worst of the three is still a floor in absolute terms
    // (each kernel uses fixed bytecode that fits in L1 i-cache; production
    // WASM with larger code working sets will be slower). Use the worst
    // ns/gas as the lower bound for the interpreted regime.
    // -----------------------------------------------------------------------

    struct AnchorStats
    {
        std::int64_t ns_small = 0;
        std::int64_t gas_small = 0;
        std::int64_t ns_large = 0;
        std::int64_t gas_large = 0;
        double ns_per_iter = 0.0;
        double gas_per_iter = 0.0;
        double ns_per_gas = 0.0;
    };

    // Run one kernel through the engine at two iteration counts and return
    // the differential ns/gas. Helpers (jtx Env, hfs) are constructed in the
    // caller and passed in so the three kernels share setup.
    AnchorStats
    measureKernel(
        Bytes const& wasm,
        HostFunctions& hfs,
        ImportVec const& imports,
        beast::Journal journal,
        std::int32_t smallIters,
        std::int32_t largeIters,
        int repeats)
    {
        auto& engine = WasmEngine::instance();
        constexpr std::int64_t kGasLimit = 1'000'000'000;

        auto runOnce =
            [&](std::int32_t iters) -> std::pair<std::int64_t, std::int64_t> {
            auto const t0 = std::chrono::steady_clock::now();
            auto const re = engine.run(
                wasm,
                hfs,
                kGasLimit,
                "kernel",
                wasmParams(iters),
                imports,
                journal);
            auto const t1 = std::chrono::steady_clock::now();
            BEAST_EXPECT(re.has_value());
            auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                t1 - t0)
                                .count();
            return {ns, re ? re.value().cost : std::int64_t{0}};
        };

        // Warmup: prime icache, branch predictors, kernel scheduler.
        for (int i = 0; i < 3; ++i)
        {
            (void)runOnce(smallIters);
            (void)runOnce(largeIters);
        }

        std::vector<std::int64_t> smallNs, smallGas, largeNs, largeGas;
        smallNs.reserve(repeats);
        smallGas.reserve(repeats);
        largeNs.reserve(repeats);
        largeGas.reserve(repeats);
        for (int i = 0; i < repeats; ++i)
        {
            auto const [ns_s, gas_s] = runOnce(smallIters);
            smallNs.push_back(ns_s);
            smallGas.push_back(gas_s);
            auto const [ns_l, gas_l] = runOnce(largeIters);
            largeNs.push_back(ns_l);
            largeGas.push_back(gas_l);
        }
        std::sort(smallNs.begin(), smallNs.end());
        std::sort(smallGas.begin(), smallGas.end());
        std::sort(largeNs.begin(), largeNs.end());
        std::sort(largeGas.begin(), largeGas.end());

        AnchorStats s;
        s.ns_small = smallNs[repeats / 2];
        s.gas_small = smallGas[repeats / 2];
        s.ns_large = largeNs[repeats / 2];
        s.gas_large = largeGas[repeats / 2];
        double const dIters = static_cast<double>(largeIters - smallIters);
        s.ns_per_iter = static_cast<double>(s.ns_large - s.ns_small) / dIters;
        s.gas_per_iter =
            static_cast<double>(s.gas_large - s.gas_small) / dIters;
        s.ns_per_gas = s.gas_per_iter > 0 ? s.ns_per_iter / s.gas_per_iter : 0.0;
        return s;
    }

    void
    printKernelResult(
        std::string const& name,
        std::string const& opsDesc,
        std::int32_t smallIters,
        std::int32_t largeIters,
        int repeats,
        AnchorStats const& s)
    {
        log << "\n";
        log << "  --- kernel: " << name << " ---\n";
        log << "    " << opsDesc << "\n";
        log << "         iters | median ns  | gas consumed\n";
        log << "    -----------+------------+--------------\n";
        log << "    " << std::setw(10) << smallIters << " | " << std::setw(10)
            << s.ns_small << " | " << s.gas_small << "\n";
        log << "    " << std::setw(10) << largeIters << " | " << std::setw(10)
            << s.ns_large << " | " << s.gas_large << "\n";
        log << "    ns/iter (diff):   " << std::fixed << std::setprecision(2)
            << s.ns_per_iter << "\n";
        log << "    gas/iter (diff):  " << s.gas_per_iter << "\n";
        log << "    ns/gas:           " << std::setprecision(4) << s.ns_per_gas
            << "\n";
    }

    void
    testInstructionAnchor()
    {
        testcase("WASM instruction anchor (wasmi engine, 6-kernel bracket)");

        // --- Kernel 1: mixed-opcode baseline (i-cache-friendly floor) ---
        // (module
        //   (memory (export "memory") 1)
        //   (func (export "kernel") (param $iters i32) (result i32)
        //     (local $acc i32) (local $tmp i32)
        //     (loop $L
        //       (local.set $acc (i32.add (local.get $acc)
        //         (i32.mul (local.get $iters) (i32.const 7))))
        //       (local.set $acc (i32.xor (local.get $acc) (i32.const 0xdeadbeef)))
        //       (i32.store (i32.const 0) (local.get $acc))
        //       (local.set $tmp (i32.load (i32.const 0)))
        //       (local.set $iters (i32.sub (local.get $iters) (i32.const 1)))
        //       (br_if $L (local.get $iters))) (local.get $acc)))
        static constexpr char kMixedHex[] =
            "0061736d0100000001060160017f017f0302010005030100010713"
            "02066d656d6f72790200066b65726e656c00000a39013701027f03"
            "402001200041076c6a2101200141effdb6f57d7321014100200136"
            "020041002802002102200041016b210020000d000b20010b";

        // --- Kernel 2: memory-scatter (LCG-driven load/store across 128 KB) ---
        // (module
        //   (memory (export "memory") 2)  ;; 128 KB
        //   (func (export "kernel") (param $iters i32) (result i32)
        //     (local $acc i32) (local $rng i32) (local $off i32)
        //     (local.set $rng (i32.const 0x12345678))
        //     (loop $L
        //       (local.set $rng (i32.add
        //         (i32.mul (local.get $rng) (i32.const 1664525))
        //         (i32.const 1013904223)))
        //       (local.set $off (i32.and (local.get $rng) (i32.const 0x1FFFC)))
        //       (local.set $acc (i32.xor (local.get $acc)
        //         (i32.load (local.get $off))))
        //       (i32.store (local.get $off) (local.get $acc))
        //       (local.set $iters (i32.sub (local.get $iters) (i32.const 1)))
        //       (br_if $L (local.get $iters))) (local.get $acc)))
        static constexpr char kScatterHex[] =
            "0061736d0100000001060160017f017f030201000503010002071302066d"
            "656d6f72790200066b65726e656c00000a49014701037f41f8acd1910121"
            "0203402002418dcce5006c41dfe6bbe3036a2102200241fcff0771210320"
            "01200328020073210120032001360200200041016b210020000d000b2001"
            "0b";

        // --- Kernel 3: branch-heavy (two unpredictable branches per iter) ---
        // (module
        //   (memory (export "memory") 1)
        //   (func (export "kernel") (param $iters i32) (result i32)
        //     (local $acc i32) (local $rng i32)
        //     (local.set $rng (i32.const 0x9E3779B9))
        //     (loop $L
        //       (local.set $rng (i32.add
        //         (i32.mul (local.get $rng) (i32.const 1664525))
        //         (i32.const 1013904223)))
        //       (if (i32.and (local.get $rng) (i32.const 1))
        //         (then (local.set $acc (i32.add (local.get $acc) (local.get $rng))))
        //         (else (local.set $acc (i32.sub (local.get $acc) (local.get $rng)))))
        //       (if (i32.and (local.get $rng) (i32.const 0x10000))
        //         (then (local.set $acc (i32.xor (local.get $acc) (i32.const 0xFFFF))))
        //         (else (local.set $acc (i32.mul (local.get $acc) (i32.const 3)))))
        //       (local.set $iters (i32.sub (local.get $iters) (i32.const 1)))
        //       (br_if $L (local.get $iters))) (local.get $acc)))
        static constexpr char kBranchesHex[] =
            "0061736d0100000001060160017f017f030201000503010001071302066d"
            "656d6f72790200066b65726e656c00000a61015f01027f41b9f3ddf17921"
            "0203402002418dcce5006c41dfe6bbe3036a210220024101710440200120"
            "026a210105200120026b21010b200241808004710440200141ffff037321"
            "0105200141036c21010b200041016b210020000d000b20010b";

        // --- Kernel 4: diverse-opcode (call + div_u + rem_u + select + ...) ---
        // (module
        //   (memory (export "memory") 2)
        //   (func $helper (param $x i32) (result i32)
        //     (i32.add (local.get $x) (i32.const 7)))
        //   (func (export "kernel") (param $iters i32) (result i32)
        //     (local $acc i32) (local $rng i32) (local $tmp i32) (local $off i32)
        //     (local.set $rng (i32.const 0xCAFE0001))
        //     (loop $L
        //       (local.set $rng (i32.add
        //         (i32.mul (local.get $rng) (i32.const 1664525))
        //         (i32.const 1013904223)))
        //       (local.set $tmp (i32.add (local.get $acc) (local.get $rng)))
        //       (local.set $tmp (i32.sub (local.get $tmp) (i32.const 17)))
        //       (local.set $tmp (i32.mul (local.get $tmp) (i32.const 13)))
        //       (local.set $tmp (i32.div_u (local.get $tmp) (i32.const 7)))
        //       (local.set $tmp (i32.rem_u (local.get $tmp) (i32.const 1024)))
        //       (local.set $tmp (i32.shl (local.get $tmp) (i32.const 3)))
        //       (local.set $tmp (i32.shr_u (local.get $tmp) (i32.const 2)))
        //       (local.set $tmp (i32.and (local.get $tmp) (i32.const 0xFF)))
        //       (local.set $tmp (i32.or (local.get $tmp) (i32.const 0x40)))
        //       (local.set $tmp (select (local.get $tmp) (i32.const 100)
        //         (i32.lt_u (local.get $tmp) (i32.const 200))))
        //       (local.set $off (i32.and (local.get $rng) (i32.const 0x1FFFC)))
        //       (i32.store (local.get $off) (local.get $tmp))
        //       (local.set $tmp (i32.load (local.get $off)))
        //       (local.set $acc (call $helper (local.get $tmp)))
        //       (if (i32.and (local.get $rng) (i32.const 1))
        //         (then (local.set $acc (i32.add (local.get $acc) (i32.const 1))))
        //         (else (local.set $acc (i32.sub (local.get $acc) (i32.const 1)))))
        //       (local.set $iters (i32.sub (local.get $iters) (i32.const 1)))
        //       (br_if $L (local.get $iters))) (local.get $acc)))
        static constexpr char kDiverseHex[] =
            "0061736d0100000001060160017f017f03030200000503010002071302"
            "066d656d6f72790200066b65726e656c00010abc01020700200041076a"
            "0bb10101047f418180f8d77c210203402002418dcce5006c41dfe6bbe3"
            "036a2102200120026a2103200341116b21032003410d6c210320034107"
            "6e210320034180087021032003410374210320034102762103200341ff"
            "01712103200341c000722103200341e400200341c801491b2103200241"
            "fcff07712104200420033602002004280200210320031000210120024101"
            "710440200141016a210105200141016b21010b200041016b210020000d"
            "000b20010b";

        // --- Kernel 5: call_indirect (8-way table dispatch, random index) ---
        // (module
        //   (memory (export "memory") 1)
        //   (type $sig (func (param i32) (result i32)))
        //   (table 8 funcref)
        //   (elem (i32.const 0) $f0 $f1 $f2 $f3 $f4 $f5 $f6 $f7)
        //   (func $f0..$f7 (param i32) (result i32) ...)
        //   (func (export "kernel") (param $iters i32) (result i32)
        //     (local $acc i32) (local $rng i32) (local $idx i32)
        //     (local.set $rng (i32.const 0xBEEF0001))
        //     (loop $L
        //       (local.set $rng (i32.add
        //         (i32.mul (local.get $rng) (i32.const 1664525))
        //         (i32.const 1013904223)))
        //       (local.set $idx (i32.and (local.get $rng) (i32.const 7)))
        //       (local.set $acc (call_indirect (type $sig)
        //                                      (local.get $acc) (local.get $idx)))
        //       (local.set $iters (i32.sub (local.get $iters) (i32.const 1)))
        //       (br_if $L (local.get $iters))) (local.get $acc)))
        static constexpr char kCallIndHex[] =
            "0061736d0100000001060160017f017f030a0900000000000000000004"
            "04017000080503010001071302066d656d6f72790200066b65726e656c"
            "0008090e010041000b0800010203040506070a7f090700200041016a0b"
            "0700200041026a0b0700200041036a0b0700200041046a0b0700200041"
            "05730b070020004106730b0700200041076b0b0700200041086b0b3d01"
            "037f418180bcf77b210203402002418dcce5006c41dfe6bbe3036a2102"
            "20024107712103200120031100002101200041016b210020000d000b20"
            "010b";

        // --- Kernel 6: combined worst-case (call_indirect + 4 params + div) ---
        // (module
        //   (memory (export "memory") 1)
        //   (type $sig (func (param i32 i32 i32 i32) (result i32)))
        //   (table 4 funcref)
        //   (elem (i32.const 0) $f0 $f1 $f2 $f3)
        //   (func $f0 .. $f3 do div_u / rem_u over 2 of the params)
        //   (func (export "kernel") (param $iters i32) (result i32)
        //     (local $acc i32) (local $rng i32) (local $idx i32)
        //     (local.set $rng (i32.const 0xDEAD0001))
        //     (loop $L
        //       (local.set $rng (LCG ...))
        //       (local.set $idx (i32.and (local.get $rng) (i32.const 3)))
        //       (local.set $acc (call_indirect (type $sig)
        //         (local.get $acc) (local.get $rng) (local.get $idx) (local.get $iters)
        //         (local.get $idx)))
        //       (local.set $iters (i32.sub (local.get $iters) (i32.const 1)))
        //       (br_if $L (local.get $iters))) (local.get $acc)))
        static constexpr char kCombinedHex[] =
            "0061736d01000000010e0260047f7f7f7f017f60017f017f0306050000"
            "00000104040170000405030100010713020"
            "66d656d6f72790200066b65726e656c0004090a010041000b04000102"
            "030a7d050d00200020016a200241076a6e0b0d00200020016b2003410d"
            "6a700b0d00200020016c200241116a6e0b0d002000200173200341176a"
            "700b4301037f418180b4f57d210203402002418dcce5006c41dfe6bbe3"
            "036a210220024103712103200120022003200020031100002101200041"
            "016b210020000d000b20010b";

        auto unhex = [](char const* hex) {
            auto const s = boost::algorithm::unhex(std::string(hex));
            return Bytes(s.begin(), s.end());
        };
        auto const mixedWasm = unhex(kMixedHex);
        auto const scatterWasm = unhex(kScatterHex);
        auto const branchesWasm = unhex(kBranchesHex);
        auto const diverseWasm = unhex(kDiverseHex);
        auto const callIndWasm = unhex(kCallIndHex);
        auto const combinedWasm = unhex(kCombinedHex);

        using namespace jtx;
        Env env{*this};

        // hfs is required by the engine API but the kernels import nothing
        // and call no host fns, so the impl is irrelevant.
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
        auto const dummyEscrow =
            keylet::escrow(env.master, env.seq(env.master));
        WasmHostFunctionsImpl hfs(ac, dummyEscrow);
        ImportVec const imports;

        constexpr std::int32_t kSmallIters = 100'000;
        constexpr std::int32_t kLargeIters = 1'000'000;
        constexpr int kRepeats = 11;

        auto const s1 = measureKernel(
            mixedWasm, hfs, imports, env.journal,
            kSmallIters, kLargeIters, kRepeats);
        auto const s2 = measureKernel(
            scatterWasm, hfs, imports, env.journal,
            kSmallIters, kLargeIters, kRepeats);
        auto const s3 = measureKernel(
            branchesWasm, hfs, imports, env.journal,
            kSmallIters, kLargeIters, kRepeats);
        auto const s4 = measureKernel(
            diverseWasm, hfs, imports, env.journal,
            kSmallIters, kLargeIters, kRepeats);
        auto const s5 = measureKernel(
            callIndWasm, hfs, imports, env.journal,
            kSmallIters, kLargeIters, kRepeats);
        auto const s6 = measureKernel(
            combinedWasm, hfs, imports, env.journal,
            kSmallIters, kLargeIters, kRepeats);

        log << "\n";
        log << "=== WASM instruction anchor (wasmi engine, 6-kernel bracket) ===\n";
        log << "  Differential: median of " << kRepeats
            << " repeats at two iteration counts, subtracted to\n";
        log << "  remove engine setup / module-instantiation overhead.\n";

        printKernelResult(
            "mixed-opcode (baseline / i-cache friendly)",
            "Ops: i32.add/mul/xor/sub, fixed-addr store/load, br_if, locals.",
            kSmallIters, kLargeIters, kRepeats, s1);
        printKernelResult(
            "memory-scatter (defeats d-cache pinning)",
            "Ops: LCG-driven scattered load/store across 128 KB linear memory.",
            kSmallIters, kLargeIters, kRepeats, s2);
        printKernelResult(
            "branch-heavy (defeats branch predictor)",
            "Ops: two data-dependent unpredictable if/else per iteration.",
            kSmallIters, kLargeIters, kRepeats, s3);
        printKernelResult(
            "diverse-opcode (div_u, rem_u, select, call, scattered mem, branch)",
            "Ops: ~17 distinct/iter; stresses handler diversity + call frame setup.",
            kSmallIters, kLargeIters, kRepeats, s4);
        printKernelResult(
            "call_indirect (8-way table dispatch, random index)",
            "Ops: table-bounds-check + runtime-type-check + indirect dispatch.",
            kSmallIters, kLargeIters, kRepeats, s5);
        printKernelResult(
            "combined-worst (call_indirect + 4 params + div_u/rem_u in callee)",
            "Ops: stacks indirect dispatch + multi-param marshaling + expensive arith.",
            kSmallIters, kLargeIters, kRepeats, s6);

        double const ns_per_gas_worst = std::max(
            {s1.ns_per_gas, s2.ns_per_gas, s3.ns_per_gas, s4.ns_per_gas,
             s5.ns_per_gas, s6.ns_per_gas});
        double const ns_per_gas_best = std::min(
            {s1.ns_per_gas, s2.ns_per_gas, s3.ns_per_gas, s4.ns_per_gas,
             s5.ns_per_gas, s6.ns_per_gas});

        log << "\n";
        log << "  Bracket summary:\n";
        log << "    best  (lowest)  ns/gas: " << std::fixed
            << std::setprecision(4) << ns_per_gas_best << "\n";
        log << "    worst (highest) ns/gas: " << ns_per_gas_worst << "\n";
        log << "    spread (worst/best):    "
            << (ns_per_gas_best > 0 ? ns_per_gas_worst / ns_per_gas_best : 0.0)
            << "x\n";
        if (nsPerGas_ > 0)
        {
            log << "\n";
            log << "  vs host-fn anchor (getLedgerSqn = " << nsPerGas_
                << " ns/gas):\n";
            log << "    worst-instr / host-fn = "
                << (ns_per_gas_worst / nsPerGas_) << "x\n";
            log << "    (Ratio >> 1: the schedule treats 1 gas of interpreted\n";
            log << "     work as MORE wall-clock than 1 gas of the cheapest\n";
            log << "     host call. Calibrate host fns against the worst\n";
            log << "     interpreted-regime number to make the schedule\n";
            log << "     internally consistent.)\n";
        }
        log << "\n";
        log << "  NOTE: even the worst of the six kernels is still a FLOOR.\n";
        log << "  Each kernel uses fixed bytecode that fits in L1 i-cache.\n";
        log << "  Production WASM with larger code working sets will be\n";
        log << "  slower; treat the worst ns/gas as the lower bound on the\n";
        log << "  interpreted regime.\n";
        log << std::endl;
    }

    void
    run() override
    {
        // Main 6 tests reported in §3.1.1 of the report.
        testGetLedgerSqnBaseline();
        testGetCurrentLedgerObjFieldSweep();
        testCacheLedgerObjSweep();
        testUpdateDataSweep();
        testComputeSha512HashSweep();
        testCheckSignatureSweep();
        //        testSingleCallStep();

        // Secondary anchor: the wasmi-instruction regime, via the engine.
        testInstructionAnchor();
    }
};

BEAST_DEFINE_TESTSUITE_MANUAL(HostFuncBenchmark, app, xrpl);

}  // namespace test
}  // namespace xrpl
