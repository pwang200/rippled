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

#include <wasm.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace xrpl {
namespace test {

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
        static constexpr std::array<std::int32_t, 5> sizes = {32, 128, 512, 1024, 4096};
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

            auto const samples = runTimedLoop(
                rt, [&]() { getCurrentLedgerObjField_wrap(&udata, &params, &results); });
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
        if (nsPerGas_ > 0)
        {
            log << "  intercept as gas:    " << (intercept / nsPerGas_)
                << "  (using baseline calibration " << nsPerGas_ << " ns/gas)\n";
            log << "  slope as gas/byte:   " << (slope / nsPerGas_) << "\n";
        }
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
        impFunc.gas = 300;
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
                [&]() { checkSignature_wrap(&udata, &params, &results); },
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
        if (nsPerGas_ > 0)
        {
            log << "  intercept as gas:    " << (intercept / nsPerGas_)
                << "  (using baseline calibration " << nsPerGas_
                << " ns/gas)\n";
            log << "  slope as gas/byte:   " << (slope / nsPerGas_) << "\n";
        }
        log << std::endl;
    }

    void
    run() override
    {
        testGetLedgerSqnBaseline();
        testGetCurrentLedgerObjFieldSweep();
        testComputeSha512HashSweep();
        testCheckSignatureSweep();
    }
};

BEAST_DEFINE_TESTSUITE_MANUAL(HostFuncBenchmark, app, xrpl);

}  // namespace test
}  // namespace xrpl
