#include <test/jtx.h>

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/wasm/HostFunc.h>
#include <xrpld/app/wasm/HostFuncImpl.h>
#include <xrpld/app/wasm/HostFuncWrapper.h>

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/Indexes.h>

#include "FuzzerSuite.h"

namespace {
struct FuzzerGlobalState
{
    std::unique_ptr<fuzzer::FuzzerSuite> suite;
    std::unique_ptr<xrpl::test::jtx::Env> env;

    FuzzerGlobalState()
    {
        suite = std::make_unique<fuzzer::FuzzerSuite>();
    }
};

FuzzerGlobalState&
getGlobalState()
{
    static FuzzerGlobalState state;
    return state;
}

// Create pre-populated ledger environment
std::unique_ptr<xrpl::test::jtx::Env>
createFuzzerEnv()
{
    using namespace xrpl::test::jtx;

    auto& state = getGlobalState();

    auto env =
        std::make_unique<Env>(*state.suite, envconfig(), testable_amendments());

    return env;
}
// Create ApplyContext for WASM execution
xrpl::ApplyContext createFuzzerApplyContext(
    xrpl::test::jtx::Env& env,
    xrpl::OpenView& ov,
    xrpl::STTx& tx)
{
    using namespace xrpl;
    using namespace xrpl::test::jtx;

    ApplyContext ac{
        env.app(),
        ov,
        tx,
        tesSUCCESS,
        env.current()->fees().base,
        tapNONE,
        env.journal
    };

    return ac;
}



}  // namespace

extern "C" int
LLVMFuzzerInitialize(int* argc, char*** argv)
{
    auto& state = getGlobalState();
    state.env = createFuzzerEnv();
    return 0;
}

struct Slice
{
    uint8_t* ptr;
    size_t len;
};

extern "C" {
    Slice
    fuzz_get_module(uint8_t const* ptr, size_t len);
    void
    fuzz_free_module(Slice slice);
}

extern "C" int
LLVMFuzzerTestOneInput(uint8_t const* ptr, size_t size)
{
    if (size < 8)
        return 0;
#ifdef FUZZ_HOST
    std::vector<uint8_t> wasm(ptr, ptr + size);
#else
    Slice module = fuzz_get_module(ptr, size);
    if (!module.ptr || module.len == 0)
        return 0;
    std::vector<uint8_t> wasm(module.ptr, module.ptr + module.len);
#endif
    auto& state = getGlobalState();
    auto const dummyEscrow =
            xrpl::keylet::escrow(state.env->master, state.env->seq(state.env->master));
    xrpl::OpenView ov{*state.env->current()};
    xrpl::STTx tx(
        xrpl::ttESCROW_FINISH,
        [&](xrpl::STObject& obj) {
            obj.setAccountID(xrpl::sfAccount, state.env->master.id());
            obj.setFieldU32(
                xrpl::sfSequence, state.env->seq(state.env->master));
            obj.setFieldAmount(
                xrpl::sfFee, state.env->current()->fees().base);
        });
    xrpl::ApplyContext ac = createFuzzerApplyContext(*state.env, ov, tx);
    xrpl::WasmHostFunctionsImpl hfs(ac, dummyEscrow);
    // Create import vector
    xrpl::ImportVec imp = createWasmImport(hfs);
    auto& engine = xrpl::WasmEngine::instance();
    auto re = engine.run(
        wasm,
        "fuzz",
        {},
        imp,
        &hfs,
        100'000,  // Instruction limit
        state.env->journal
    );
#ifndef FUZZ_HOST
    fuzz_free_module(module);
#endif
    return 0;
}
