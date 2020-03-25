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
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/beast/unit_test.h>
#include <ripple/app/tx/apply.h>
#include <test/jtx.h>
#include <vector>

namespace ripple {
namespace test {

class NegativeUNL_test : public beast::unit_test::suite
{
    void
    testNegativeUNL()
    {
        jtx::Env env(*this);

        Config config;
        auto l = std::make_shared<Ledger>(
                create_genesis, config,
                std::vector<uint256>{}, env.app().family());

        l = std::make_shared<Ledger>(
                *l,
                env.app().timeKeeper().closeTime());

        NodeID n1(0xA1);
        NodeID n2(0xA2);
        //NodeID n3(0xA3);
        //NodeID e1(0xE1);
        //NodeID e2(0xE2);
        //NodeID e3(0xE3);
        NodeID badfood(0xBADDF00D);

        bool adding;
        NodeID txNodeId;
        auto fill = [&](auto &obj)
        {
            obj.setFieldU8(sfNegativeUNLTxAdd, adding ? 1 : 0);
            obj.setFieldU32(sfLedgerSequence, l->seq());
            obj.setFieldH160(sfNegativeUNLTxNodeID, txNodeId);
            std::cout << txNodeId << std::endl;
        };

        adding = true;
        txNodeId = n1;
        STTx txAdd(ttNEGATIVE_UNL, fill);
        txNodeId = n2;
        STTx txAdd_2(ttNEGATIVE_UNL, fill);
        txNodeId = badfood;
        STTx txAdd_bad(ttNEGATIVE_UNL, fill);

        adding = false;
        txNodeId = n1;
        STTx txRemove(ttNEGATIVE_UNL, fill);
        txNodeId = n2;
        STTx txRemove_2(ttNEGATIVE_UNL, fill);
        txNodeId = badfood;
        STTx txRemove_bad(ttNEGATIVE_UNL, fill);

        auto applyAndTestResult = [&](OpenView& view, STTx const& tx, bool pass) -> bool
        {
            auto res = apply(env.app(), view, tx, ApplyFlags::tapNONE, env.journal);
            if(pass)
                return res.first == tesSUCCESS;
            else
                return res.first == tefFAILURE;
        };

        auto nUnlSizeTest = [&](size_t size, bool hasToAdd, bool hasToRemove) -> bool
        {
            bool sameSize = l->negativeUNL().size() == size;
            if (!sameSize)
            {
                JLOG (env.journal.debug()) << "negativeUNL size,"
                           << " expect " << size
                           << " actual " << l->negativeUNL().size();
            }
            bool sameToAdd = (l->negativeUNLToAdd() != std::nullopt) == hasToAdd;
            if(!sameToAdd)
            {
                JLOG (env.journal.debug()) << "negativeUNL has ToAdd,"
                           << " expect " << hasToAdd
                           << " actual " << (l->negativeUNLToAdd() != std::nullopt);
            }
            bool sameToRemove = (l->negativeUNLToRemove() != std::nullopt) == hasToRemove;
            if(!sameToRemove)
            {
                JLOG (env.journal.debug()) << "negativeUNL has ToRemove,"
                           << " expect " << hasToRemove
                           << " actual " << (l->negativeUNLToRemove() != std::nullopt);
            }

            return sameSize && sameToAdd && sameToRemove;
        };

        /*
         * test cases:
         *
         * (1) the ledger after genesis
         * -- cannot apply Add Tx
         * -- cannot apply Remove Tx
         * -- nUNL empty
         * -- no ToAdd
         * -- no ToRemove
         *
         * (2) a flag ledger
         * -- apply an Add Tx
         * -- cannot apply the second Add Tx
         * -- cannot apply a Remove Tx
         * -- nUNL empty
         * -- has ToAdd with right nodeId
         * -- no ToRemove
         *
         * (3) ledgers before the next flag ledger
         * -- nUNL empty
         * -- has ToAdd with right nodeId
         * -- no ToRemove
         *
         * (4) next flag ledger
         * -- nUNL size == 1, with right nodeId
         * -- no ToAdd
         * -- no ToRemove
         * -- cannot apply an Add Tx with nodeId already in nUNL
         * -- apply an Add Tx with different nodeId
         * -- cannot apply a Remove Tx with the same NodeId as Add
         * -- cannot apply a Remove Tx with a NodeId not in nUNL
         * -- apply a Remove Tx with a nodeId already in nUNL
         * -- has ToAdd with right nodeId
         * -- has ToRemove with right nodeId
         * -- nUNL size still 1, right nodeId
         *
         * (5) ledgers before the next flag ledger
         * -- nUNL size == 1, right nodeId
         * -- has ToAdd with right nodeId
         * -- has ToRemove with right nodeId
         *
         * (6) next flag ledger
         * -- nUNL size == 1, different nodeId
         * -- no ToAdd
         * -- no ToRemove
         * -- apply an Add Tx with different nodeId
         * -- nUNL size still 1, right nodeId
         * -- has ToAdd with right nodeId
         * -- no ToRemove
         *
         * (7) ledgers before the next flag ledger
         * -- nUNL size still 1, right nodeId
         * -- has ToAdd with right nodeId
         * -- no ToRemove
         *
         * (8) next flag ledger
         * -- nUNL size == 2
         * -- apply a Remove Tx
         * -- cannot apply second Remove Tx, even with right nodeId
         * -- cannot apply an Add Tx with the same NodeId as Remove
         * -- nUNL size == 2
         * -- no ToAdd
         * -- has ToRemove with right nodeId
         *
         * (9) ledgers before the next flag ledger
         * -- nUNL size == 2
         * -- no ToAdd
         * -- has ToRemove with right nodeId
         *
         * (10) next flag ledger
         * -- nUNL size == 1
         * -- apply a Remove Tx
         * -- nUNL size == 1
         * -- no ToAdd
         * -- has ToRemove with right nodeId
         *
         * (11) ledgers before the next flag ledger
         * -- nUNL size == 1
         * -- no ToAdd
         * -- has ToRemove with right nodeId
         *
         * (12) next flag ledger
         * -- nUNL size == 0
         * -- no ToAdd
         * -- no ToRemove
         *
         * (13) ledgers before the next flag ledger
         * -- nUNL size == 0
         * -- no ToAdd
         * -- no ToRemove
         *
         * (14) next flag ledger
         * -- nUNL size == 0
         * -- no ToAdd
         * -- no ToRemove
         */

        {
            //(1) the ledger after genesis, not a flag ledger
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove, false));
            accum.apply(*l);
            BEAST_EXPECT(nUnlSizeTest(0, false, false));
        }

        {
            //(2) a flag ledger
            //more ledgers
            for (auto i = 0; i < FLAG_LEDGER - 2; ++i)
            {
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }
            //flag ledger now
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, true));
            BEAST_EXPECT(applyAndTestResult(accum, txAdd_2, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove, false));
            accum.apply(*l);
            auto good_size = nUnlSizeTest(0, true, false);
            BEAST_EXPECT(good_size);
            if (good_size)
                BEAST_EXPECT(l->negativeUNLToAdd() == n1);
        }

        {
            //(3) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(0, true, false);
                BEAST_EXPECT(good_size);
                if (good_size)
                    BEAST_EXPECT(l->negativeUNLToAdd() == n1);
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(4) next flag ledger
            auto good_size = nUnlSizeTest(1, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
                BEAST_EXPECT(*(l->negativeUNL().begin()) == n1);
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, false));
            BEAST_EXPECT(applyAndTestResult(accum, txAdd_2, true));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_2, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_bad, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove, true));
            accum.apply(*l);
            good_size = nUnlSizeTest(1, true, true);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n1) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToAdd() == n2);
                BEAST_EXPECT(l->negativeUNLToRemove() == n1);
            }
        }

        {
            //(5) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(1, true, true);
                BEAST_EXPECT(good_size);
                if(good_size)
                {
                    BEAST_EXPECT(l->negativeUNL().find(n1) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToAdd() == n2);
                    BEAST_EXPECT(l->negativeUNLToRemove() == n1);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(6) next flag ledger
            auto good_size = nUnlSizeTest(1, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
            }
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, true));
            accum.apply(*l);
            good_size = nUnlSizeTest(1, true, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToAdd() == n1);
            }
        }

        {
            //(7) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(1, true, false);
                BEAST_EXPECT(good_size);
                if(good_size)
                {
                    BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToAdd() == n1);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(8) next flag ledger
            auto good_size = nUnlSizeTest(2, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n1) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
            }
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txRemove, true));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_2, false));
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, false));
            accum.apply(*l);
            good_size = nUnlSizeTest(2, false, true);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n1) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToRemove() == n1);
            }
        }

        {
            //(9) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(2, false, true);
                BEAST_EXPECT(good_size);
                if(good_size)
                {
                    BEAST_EXPECT(l->negativeUNL().find(n1) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToRemove() == n1);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(10) next flag ledger
            auto good_size = nUnlSizeTest(1, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
            }
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_2, true));
            accum.apply(*l);
            good_size = nUnlSizeTest(1, false, true);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToRemove() == n2);
            }
        }

        {
            //(11) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(1, false, true);
                BEAST_EXPECT(good_size);
                if(good_size)
                {
                    BEAST_EXPECT(l->negativeUNL().find(n2) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToRemove() == n2);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(12) next flag ledger
            auto good_size = nUnlSizeTest(0, false, false);
            BEAST_EXPECT(good_size);
        }

        {
            //(13) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(0, false, false);
                BEAST_EXPECT(good_size);
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(14) next flag ledger
            auto good_size = nUnlSizeTest(0, false, false);
            BEAST_EXPECT(good_size);
        }
    }

    void run() override
    {
        testNegativeUNL();
    }
};

BEAST_DEFINE_TESTSUITE(NegativeUNL,ledger,ripple);

}  // test
}  // ripple
