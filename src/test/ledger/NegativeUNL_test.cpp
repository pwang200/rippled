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

namespace ripple {
namespace test {

class NegativeUNL_test : public beast::unit_test::suite
{
    void
    testNegativeUNL()
    {
        testcase ("Create UNLModify Tx and apply to ledgers");
        jtx::Env env(*this);
        env.app().logs().threshold(beast::severities::kAll);
        Config config;
        auto l = std::make_shared<Ledger>(
                create_genesis, config,
                std::vector<uint256>{}, env.app().family());

        l = std::make_shared<Ledger>(
                *l,
                env.app().timeKeeper().closeTime());

        bool adding;
        auto keyPair_1 = randomKeyPair(KeyType::ed25519);
        auto pk1 = keyPair_1.first;
        auto keyPair_2 = randomKeyPair(KeyType::ed25519);
        auto pk2 = keyPair_2.first;
        auto keyPair_3 = randomKeyPair(KeyType::ed25519);
        auto pk3 = keyPair_3.first;

        PublicKey txKey;
        auto fill = [&](auto &obj)
        {
            obj.setFieldU8(sfUNLModifyDisabling, adding ? 1 : 0);
            obj.setFieldU32(sfLedgerSequence, l->seq());
            std::cout << "fill: seq="<< l->seq() << std::endl;
            obj.setFieldVL(sfUNLModifyValidator, txKey);
            std::cout << txKey << std::endl;
        };
//
//        adding = true;
//        txKey = pk1;
//        STTx txAdd(ttUNL_MODIDY, fill);
//        txKey = pk2;
//        STTx txAdd_2(ttUNL_MODIDY, fill);
//        txKey = pkBad;
//        STTx txAdd_bad(ttUNL_MODIDY, fill);
//
//        adding = false;
//        txKey = pk1;
//        STTx txRemove(ttUNL_MODIDY, fill);
//        txKey = pk2;
//        STTx txRemove_2(ttUNL_MODIDY, fill);
//        txKey = pkBad;
//        STTx txRemove_bad(ttUNL_MODIDY, fill);

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
            bool sameToAdd = (l->negativeUNLToDisable() != boost::none) == hasToAdd;
            if(!sameToAdd)
            {
                JLOG (env.journal.debug()) << "negativeUNL has ToAdd,"
                           << " expect " << hasToAdd
                           << " actual " << (l->negativeUNLToDisable() != boost::none);
            }
            bool sameToRemove = (l->negativeUNLToReEnable() != boost::none) == hasToRemove;
            if(!sameToRemove)
            {
                JLOG (env.journal.debug()) << "negativeUNL has ToRemove,"
                           << " expect " << hasToRemove
                           << " actual " << (l->negativeUNLToReEnable() != boost::none);
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
         * ++ an extra test: first Add Tx in ledger TxSet
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
            adding = true;
            txKey = pk1;
            STTx txAdd(ttUNL_MODIDY, fill);
            adding = false;
            txKey = pk2;
            STTx txRemove_2(ttUNL_MODIDY, fill);

            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_2, false));
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
            adding = true;
            txKey = pk1;
            STTx txAdd(ttUNL_MODIDY, fill);
            txKey = pk2;
            STTx txAdd_2(ttUNL_MODIDY, fill);
            adding = false;
            txKey = pk3;
            STTx txRemove_3(ttUNL_MODIDY, fill);

            l->info().txHash;
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, true));
            BEAST_EXPECT(applyAndTestResult(accum, txAdd_2, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_3, false));
            accum.apply(*l);
            auto good_size = nUnlSizeTest(0, true, false);
            BEAST_EXPECT(good_size);
            if (good_size)
            {
                BEAST_EXPECT(l->negativeUNLToDisable() == pk1);
                //++ first Add Tx in ledger TxSet
                uint256 txID = txAdd.getTransactionID();
                BEAST_EXPECT(l->txExists(txID));
            }
//            {
//                std::cout << "XXXX seq="<< l->seq() << " hash " << l->info().hash << std::endl;
//                std::cout << "XXXX seq="<< l->seq() << " tx hash " << l->info().txHash << std::endl;
//                l->setImmutable(config);
//                std::cout << "XXXX seq="<< l->seq() << " hash " << l->info().hash << std::endl;
//                std::cout << "XXXX seq="<< l->seq() << " tx hash " << l->info().txHash << std::endl;
//            }
        }

        {
            //(3) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(0, true, false);
                BEAST_EXPECT(good_size);
                if (good_size)
                    BEAST_EXPECT(l->negativeUNLToDisable() == pk1);
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
//                if(i < FLAG_LEDGER - 1)
//                {
//                    std::cout << "XXXX seq="<< l->seq() << " hash " << l->info().hash << std::endl;
//                    std::cout << "XXXX seq="<< l->seq() << " tx hash " << l->info().txHash << std::endl;
//                    l->setImmutable(config);
//                    std::cout << "XXXX seq="<< l->seq() << " hash " << l->info().hash << std::endl;
//                    std::cout << "XXXX seq="<< l->seq() << " tx hash " << l->info().txHash << std::endl;
//                }
            }

            //(4) next flag ledger
            adding = true;
            txKey = pk1;
            STTx txAdd(ttUNL_MODIDY, fill);
            txKey = pk2;
            STTx txAdd_2(ttUNL_MODIDY, fill);
            adding = false;
            txKey = pk1;
            STTx txRemove(ttUNL_MODIDY, fill);
            txKey = pk2;
            STTx txRemove_2(ttUNL_MODIDY, fill);
            txKey = pk3;
            STTx txRemove_3(ttUNL_MODIDY, fill);
            auto good_size = nUnlSizeTest(1, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
                BEAST_EXPECT(*(l->negativeUNL().begin()) == pk1);
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, false));
            BEAST_EXPECT(applyAndTestResult(accum, txAdd_2, true));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_2, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_3, false));
            BEAST_EXPECT(applyAndTestResult(accum, txRemove, true));
            accum.apply(*l);
            good_size = nUnlSizeTest(1, true, true);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(pk1) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToDisable() == pk2);
                BEAST_EXPECT(l->negativeUNLToReEnable() == pk1);
            }
//            {
//                std::cout << "XXXX seq="<< l->seq() << " hash " << l->info().hash << std::endl;
//                std::cout << "XXXX seq="<< l->seq() << " tx hash " << l->info().txHash << std::endl;
//                l->setImmutable(config);
//                std::cout << "XXXX seq="<< l->seq() << " hash " << l->info().hash << std::endl;
//                std::cout << "XXXX seq="<< l->seq() << " tx hash " << l->info().txHash << std::endl;
//            }
        }

        {
            //(5) ledgers before the next flag ledger
            for (auto i = 0; i < FLAG_LEDGER; ++i)
            {
                auto good_size = nUnlSizeTest(1, true, true);
                BEAST_EXPECT(good_size);
                if(good_size)
                {
                    BEAST_EXPECT(l->negativeUNL().find(pk1) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToDisable() == pk2);
                    BEAST_EXPECT(l->negativeUNLToReEnable() == pk1);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(6) next flag ledger
            adding = true;
            txKey = pk1;
            STTx txAdd(ttUNL_MODIDY, fill);
            auto good_size = nUnlSizeTest(1, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
            }
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txAdd, true));
            accum.apply(*l);
            good_size = nUnlSizeTest(1, true, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToDisable() == pk1);
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
                    BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToDisable() == pk1);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(8) next flag ledger
            adding = true;
            txKey = pk1;
            STTx txAdd(ttUNL_MODIDY, fill);
            adding = false;
            txKey = pk1;
            STTx txRemove(ttUNL_MODIDY, fill);
            txKey = pk2;
            STTx txRemove_2(ttUNL_MODIDY, fill);

            auto good_size = nUnlSizeTest(2, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(pk1) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
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
                BEAST_EXPECT(l->negativeUNL().find(pk1) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToReEnable() == pk1);
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
                    BEAST_EXPECT(l->negativeUNL().find(pk1) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToReEnable() == pk1);
                }
                auto next = std::make_shared<Ledger>(
                        *l,
                        env.app().timeKeeper().closeTime());
                l = next;
            }

            //(10) next flag ledger
            adding = false;
            txKey = pk2;
            STTx txRemove_2(ttUNL_MODIDY, fill);
            auto good_size = nUnlSizeTest(1, false, false);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
            }
            OpenView accum(&*l);
            BEAST_EXPECT(applyAndTestResult(accum, txRemove_2, true));
            accum.apply(*l);
            good_size = nUnlSizeTest(1, false, true);
            BEAST_EXPECT(good_size);
            if(good_size)
            {
                BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
                BEAST_EXPECT(l->negativeUNLToReEnable() == pk2);
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
                    BEAST_EXPECT(l->negativeUNL().find(pk2) != l->negativeUNL().end());
                    BEAST_EXPECT(l->negativeUNLToReEnable() == pk2);
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
