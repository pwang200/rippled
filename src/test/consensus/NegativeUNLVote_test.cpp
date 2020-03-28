//-----------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2020 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/misc/NegativeUNLVote.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/beast/unit_test.h>
#include <ripple/app/tx/apply.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class NegativeUNLVote_test : public beast::unit_test::suite
{
    void
    testNegativeUNLVote()
    {
        jtx::Env env(*this);
        env.app().logs().threshold(beast::severities::kAll);
        std::vector<NodeID> nodeIDs;
        hash_set<NodeID> UNL;
        for(int i = 0; i < 10; ++i)
        {
            nodeIDs.emplace_back(0xA0+i);
            UNL.emplace(0xA0+i);
        }
        NodeID myId = nodeIDs[3]; //3 is arbitrary
        RCLValidations & validations = env.app().getValidations();
        beast::Journal const & journal = env.journal;
        Config config;

        auto createSTVal = [&](std::shared_ptr<Ledger> ledger, unsigned int i)
                -> STValidation::pointer
        {
            static auto keyPair = randomKeyPair (KeyType::secp256k1);
            static uint256 consensusHash;
            static STValidation::FeeSettings fees;
            static std::vector<uint256> amendments;
            return std::make_shared<STValidation>(ledger->info().hash, ledger->seq(),
                    consensusHash, env.app().timeKeeper().now(), keyPair.first,
                    keyPair.second, nodeIDs[i], true, fees, amendments);
        };

        auto nUnlSizeTest = [&](std::shared_ptr<Ledger> l,
                size_t size, bool hasToAdd, bool hasToRemove) -> bool
        {
            bool sameSize = l->negativeUNL().size() == size;
            if (!sameSize)
            {
                JLOG (env.journal.warn()) << "negativeUNL size,"
                                           << " expect " << size
                                           << " actual " << l->negativeUNL().size();
            }
            bool sameToAdd = (l->negativeUNLToAdd() != std::nullopt) == hasToAdd;
            if(!sameToAdd)
            {
                JLOG (env.journal.warn()) << "negativeUNL has ToAdd,"
                                           << " expect " << hasToAdd
                                           << " actual " << (l->negativeUNLToAdd() != std::nullopt);
            }
            bool sameToRemove = (l->negativeUNLToRemove() != std::nullopt) == hasToRemove;
            if(!sameToRemove)
            {
                JLOG (env.journal.warn()) << "negativeUNL has ToRemove,"
                                           << " expect " << hasToRemove
                                           << " actual " << (l->negativeUNLToRemove() != std::nullopt);
            }

            return sameSize && sameToAdd && sameToRemove;
        };

        auto countTx = [](std::shared_ptr<SHAMap> const& txSet)
                -> unsigned int
        {
            unsigned int count = 0;
            for(auto i = txSet->begin(); i != txSet->end(); ++i)
            {
                ++count;
            }
            return count;
        };

        /*
         * only reasonable values can be honored,
         * e.g cannot hasToRemove when nUNLSize == 0
         */
        using LedgerHistory = std::vector<std::shared_ptr<Ledger>>;
        auto createLedgerHistory = [&](LedgerHistory & history,
                int nUNLSize, bool hasToAdd, bool hasToRemove)
                -> bool
        {
            auto l = std::make_shared<Ledger>(
                    create_genesis, config,
                    std::vector<uint256>{}, env.app().family());
            history.push_back(l);

            bool adding = true;
            int nidx = 0;
            auto fill = [&](auto &obj)
            {
                obj.setFieldU8(sfNegativeUNLTxAdd, adding ? 1 : 0);
                obj.setFieldU32(sfLedgerSequence, l->seq());
                obj.setFieldH160(sfNegativeUNLTxNodeID, nodeIDs[nidx]);
                std::cout << nodeIDs[nidx] << std::endl;
            };

            auto applyAndTestResult = [&](OpenView& view, STTx const& tx, bool pass) -> bool
            {
                auto res = apply(env.app(), view, tx, ApplyFlags::tapNONE, env.journal);
                if(pass)
                    return res.first == tesSUCCESS;
                else
                    return res.first == tefFAILURE;
            };

            int numLedgers = FLAG_LEDGER * (nUNLSize + 1);
            while(l->seq() < numLedgers)
            {
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
                history.push_back(l);

                if(l->seq() % FLAG_LEDGER == 0)
                {
                    OpenView accum(&*l);
                    if( l->negativeUNL().size() < nUNLSize )
                    {
                        STTx tx(ttNEGATIVE_UNL, fill);
                        if(!applyAndTestResult(accum, tx, true))
                            break;
                        ++nidx;
                    }
                    else if (l->negativeUNL().size() == nUNLSize)
                    {
                        if(hasToAdd)
                        {
                            STTx tx(ttNEGATIVE_UNL, fill);
                            if(!applyAndTestResult(accum, tx, true))
                                break;
                            ++nidx;
                        }
                        if(hasToRemove)
                        {
                            adding = false;
                            nidx = 0;
                            STTx tx(ttNEGATIVE_UNL, fill);
                            if(!applyAndTestResult(accum, tx, true))
                                break;
                        }
                    }
                    accum.apply(*l);
                }
                l->updateSkipList ();
            }
            return nUnlSizeTest(l, nUNLSize, hasToAdd, hasToRemove);
        };

        /*
         * 1. no skip list
         * 2. short skip list
         *
         * == score table and measurement ==
         * 3. local node not enough history,
         *    -- test with a case that should create tx
         * 4. local node double validated some seq
         *    -- test with a case that should create tx
         * 5. lowWaterMark, one more, one less
         * 6. highWaterMark, one more, one less
         *
         * == negative UNL component in prevLedger ==
         * -- size 0, 1, ... maxNegativeListed + 1
         * -- hasToAdd, hasToRemove
         *
         * == UNL size ==
         * -- 10
         * -- 34
         * -- 35
         * -- 37
         * -- 40
         * -- 100
         *
         * == UNL change ==
         * -- add validators, 1 to 20% of UNL
         * -- remove validators, 2, in or not in nUNL
         *
         * 7. hit maxNegativeListed
         * ? test tx deserialize? probably don't need to
         * (0, false, false),
         */

        {
            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, 1, false, false);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    unsigned int unlSize = UNL.size();
                    for(unsigned int i = 0; i < (unlSize - 1); ++i)
                    {
                        RCLValidation v(createSTVal(l, i));
                        validations.add(nodeIDs[i], v);
                    }
                }

                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                doNegativeUNLVoting (myId,
                                     history.back(),
                                     UNL,
                                     validations,
                                     txSet,
                                     journal);

                BEAST_EXPECT(countTx(txSet) == 2);
            }
        }
    }

    void run() override
    {
        testNegativeUNLVote();
    }
};

BEAST_DEFINE_TESTSUITE(NegativeUNLVote,consensus,ripple);

}  // test
}  // ripple
