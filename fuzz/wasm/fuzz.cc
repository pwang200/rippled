#include <test/jtx.h>
#include <test/jtx/Oracle.h>

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

// Identifiers computed during ledger setup that the fuzzer iteration needs.
struct LedgerIds
{
    xrpl::Keylet escrowKeylet;
    xrpl::AccountID alice;
    xrpl::AccountID bob;
    xrpl::AccountID carol;
    xrpl::AccountID gw;
};

struct FuzzerGlobalState
{
    std::unique_ptr<fuzzer::FuzzerSuite> suite;
    std::unique_ptr<xrpl::test::jtx::Env> env;
    LedgerIds ids;

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

// Populate ledger with diverse object types so that host functions
// exercise success paths instead of returning NOT_FOUND.
// Returns identifiers needed by the fuzzer iteration.
LedgerIds
fundEnv(xrpl::test::jtx::Env& env);

extern "C" int
LLVMFuzzerInitialize(int* argc, char*** argv)
{
    using namespace xrpl::test::jtx;
    auto& state = getGlobalState();
    state.env = createFuzzerEnv();
    state.ids = fundEnv(*state.env);

    return 0;
}

LedgerIds
fundEnv(xrpl::test::jtx::Env& env)
{
    using namespace xrpl::test::jtx;

    Account const alice("alice");
    Account const bob("bob");
    Account const carol("carol");
    Account const gw("gateway");

    LedgerIds ids;
    ids.alice = alice.id();
    ids.bob = bob.id();
    ids.carol = carol.id();
    ids.gw = gw.id();

    // --- Accounts (4) ---
    env.fund(XRP(100'000), alice, bob, carol, gw);
    env.close();

    // --- Trust lines & IOU balances ---
    auto const USD = gw["USD"];
    env.trust(USD(10'000), alice);
    env.trust(USD(10'000), bob);
    env(pay(gw, alice, USD(5'000)));
    env(pay(gw, bob, USD(2'000)));
    env.close();

    // --- Escrow (capture alice's sequence before creation) ---
    auto const escrowSeq = env.seq(alice);
    auto const finishTime = env.now() + std::chrono::seconds(1);
    env.apply(
        escrow::create(alice, bob, XRP(100)), escrow::finish_time(finishTime));
    env.close();
    ids.escrowKeylet = xrpl::keylet::escrow(alice.id(), escrowSeq);

    // --- Offer (DEX) ---
    env(offer(alice, USD(50), XRP(500)));
    env.close();

    // --- NFToken + NFT sell offer ---
    auto const nftID = token::getNextID(env, alice, 0u, tfTransferable);
    env(token::mint(alice, 0u), txflags(tfTransferable));
    env.close();
    env(token::createOffer(alice, nftID, XRP(10)),
        txflags(tfSellNFToken));
    env.close();

    // --- Payment channel ---
    env(paychan::create(
        alice, bob, XRP(50), std::chrono::seconds(100), alice.pk()));
    env.close();

    // --- Check ---
    env(check::create(alice, bob, XRP(25)));
    env.close();

    // --- Deposit preauth ---
    env(deposit::auth(bob, alice));
    env.close();

    // --- Signer list ---
    env(signers(alice, 2, {{bob, 1}, {carol, 1}}));
    env.close();

    // --- Tickets ---
    env(ticket::create(alice, 2));
    env.close();

    // --- DID ---
    env(did::setValid(alice));
    env.close();

    // --- Credentials ---
    env(credentials::create(alice, gw, "fuzz_cred"));
    env.close();
    env(credentials::accept(alice, gw, "fuzz_cred"));
    env.close();

    // --- Oracle ---
    oracle::Oracle(
        env,
        {.owner = alice.id(),
         .documentID = 1,
         .series = {{"XRP", "USD", 740, 1}},
         .assetClass = "currency",
         .provider = "provider"});

    return ids;
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
    auto const& ids = state.ids;
    fundEnv(*state.env);

    // Use the REAL escrow keylet so get_current_ledger_obj_field() works.
    xrpl::OpenView ov{*state.env->current()};

    // Transaction with real account IDs so get_tx_field() returns
    // meaningful data.  Object discovery is handled by the autarkie
    // preamble (keylet computation + cache), not by tx fields.
    xrpl::STTx tx(
        xrpl::ttESCROW_FINISH,
        [&](xrpl::STObject& obj) {
            obj.setAccountID(xrpl::sfAccount, ids.alice);
            obj.setFieldU32(
                xrpl::sfSequence, state.env->seq(state.env->master));
            obj.setFieldAmount(
                xrpl::sfFee, state.env->current()->fees().base);
            obj.setAccountID(xrpl::sfDestination, ids.bob);
            obj.setAccountID(xrpl::sfOwner, ids.alice);
            obj.setFieldAmount(xrpl::sfAmount, xrpl::XRP(100));
        });

    xrpl::ApplyContext ac = createFuzzerApplyContext(*state.env, ov, tx);
    xrpl::WasmHostFunctionsImpl hfs(ac, ids.escrowKeylet);
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
