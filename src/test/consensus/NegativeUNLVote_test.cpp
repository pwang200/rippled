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
    unsigned int countTx(std::shared_ptr<SHAMap> const& txSet)
    {
        unsigned int count = 0;
        for(auto i = txSet->begin(); i != txSet->end(); ++i)
        {
            ++count;
        }
        return count;
    };

    STValidation::pointer createSTVal(jtx::Env &env,
            std::shared_ptr<Ledger> ledger, NodeID const& n)
    {
        static auto keyPair = randomKeyPair (KeyType::secp256k1);
        static uint256 consensusHash;
        static STValidation::FeeSettings fees;
        static std::vector<uint256> amendments;
        return std::make_shared<STValidation>(ledger->info().hash, ledger->seq(),
                consensusHash, env.app().timeKeeper().now(), keyPair.first,
                keyPair.second, n, true, fees, amendments);
    };

    bool nUnlSizeTest(jtx::Env &env, std::shared_ptr<Ledger> l,
                      size_t size, bool hasToAdd, bool hasToRemove)
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

    void createNodeIDs(int numNodes,
            std::vector<NodeID> & nodeIDs, hash_set<NodeID> & UNL)
    {
        for(int i = 0; i < numNodes; ++i)
        {
            nodeIDs.emplace_back(0xA000+i);
            UNL.emplace(0xA000+i);
        }
    }

    /*
     * only reasonable values can be honored,
     * e.g cannot hasToRemove when nUNLSize == 0
     */
    using LedgerHistory = std::vector<std::shared_ptr<Ledger>>;
    bool createLedgerHistory(LedgerHistory & history,//out
            jtx::Env &env,
            std::vector<NodeID> const& nodeIDs,
            int nUNLSize,
            bool hasToAdd,
            bool hasToRemove,
            int numLedgers = 0)
    {
        Config config;
        static uint256 fake_amemdment;
        auto l = std::make_shared<Ledger>(
                create_genesis, config,
                std::vector<uint256>{fake_amemdment++}, env.app().family());
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

        if(! numLedgers)
            numLedgers = FLAG_LEDGER * (nUNLSize + 1);
        std::cout << "ledger seq=" << l->seq() << std::endl;
        while(l->seq() <= numLedgers)
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
        std::cout << "ledger seq=" << l->seq() << std::endl;
        return nUnlSizeTest(env, l, nUNLSize, hasToAdd, hasToRemove);
    }

    void
    testAddTx()
    {
        jtx::Env env(*this);
        //env.app().logs().threshold(beast::severities::kAll);
        NodeID myId(0xA0);
        RCLValidations & validations = env.app().getValidations();
        NegativeUNLVote vote(myId, validations, env.journal);

        //one add, one remove
        auto txSet = std::make_shared<SHAMap>(
                SHAMapType::TRANSACTION, env.app().family());
        NodeID addId(0xA1);
        NodeID removeId(0xA2);
        LedgerIndex seq(1234);
        BEAST_EXPECT(countTx(txSet) == 0);
        vote.addTx(seq, addId, true, txSet);
        BEAST_EXPECT(countTx(txSet) == 1);
        vote.addTx(seq, removeId, false, txSet);
        BEAST_EXPECT(countTx(txSet) == 2);
        //content of a tx is implicitly tested after applied to a ledger
        //in later test cases
    }

    void
    testPickOneCandidate()
    {
        jtx::Env env(*this);
        //env.app().logs().threshold(beast::severities::kAll);
        NodeID myId(0xA0);
        RCLValidations & validations = env.app().getValidations();
        NegativeUNLVote vote(myId, validations, env.journal);

        uint256 pad_0(0);
        uint256 pad_f = ~pad_0;
        NodeID n_1(1);
        NodeID n_2(2);
        NodeID n_3(3);
        std::vector<NodeID> candidates({n_1});
        BEAST_EXPECT(vote.pickOneCandidate(pad_0, candidates) == n_1);
        BEAST_EXPECT(vote.pickOneCandidate(pad_f, candidates) == n_1);
        candidates.emplace_back(2);
        BEAST_EXPECT(vote.pickOneCandidate(pad_0, candidates) == n_1);
        BEAST_EXPECT(vote.pickOneCandidate(pad_f, candidates) == n_2);
        candidates.emplace_back(3);
        BEAST_EXPECT(vote.pickOneCandidate(pad_0, candidates) == n_1);
        BEAST_EXPECT(vote.pickOneCandidate(pad_f, candidates) == n_3);
    }

    void
    testBuildScoreTable()
    {
        /*
         * 1. no skip list
         * 2. short skip list
         * 3. local node not enough history
         * 4. local node double validated some seq
         * 5. local node good history, but not a validator,
         */
        {
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations & validations = env.app().getValidations();

            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(10, nodeIDs, UNL);
            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false, 1);
            BEAST_EXPECT(goodHistory);
            if (goodHistory)
            {
                NodeID myId = nodeIDs[3];
                NegativeUNLVote vote(myId, validations, env.journal);
                hash_map<NodeID, unsigned int> scoreTable;
                BEAST_EXPECT(!vote.buildScoreTable(history[0], UNL, scoreTable));
            }
        }

        {
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations & validations = env.app().getValidations();

            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(10, nodeIDs, UNL);
            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false, FLAG_LEDGER / 2);
            BEAST_EXPECT(goodHistory);
            if (goodHistory)
            {
                NodeID myId = nodeIDs[3];
                NegativeUNLVote vote(myId, validations, env.journal);
                hash_map<NodeID, unsigned int> scoreTable;
                BEAST_EXPECT(!vote.buildScoreTable(history.back(), UNL, scoreTable));
            }
        }

        {
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations & validations = env.app().getValidations();

            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(10, nodeIDs, UNL);
            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                             0, false, false, FLAG_LEDGER + 2);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                NodeID myId = nodeIDs[3];
                for (auto &l : history)
                {
                    unsigned int unlSize = UNL.size();
                    for (unsigned int i = 0; i < unlSize; ++i)
                    {
                        if(nodeIDs[i] == myId && l->seq() % 2 == 0)
                            continue;
                        RCLValidation v(createSTVal(env, l, nodeIDs[i]));
                        validations.add(nodeIDs[i], v);
                    }
                }
                NegativeUNLVote vote(myId, validations, env.journal);
                hash_map<NodeID, unsigned int> scoreTable;
                BEAST_EXPECT(!vote.buildScoreTable(history.back(), UNL, scoreTable));
            }
        }

        {
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations & validations = env.app().getValidations();

            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(10, nodeIDs, UNL);

            std::shared_ptr<Ledger const> firstRound;
            {
                LedgerHistory history;
                bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                       0, false, false, FLAG_LEDGER + 2);
                BEAST_EXPECT(goodHistory);
                if(goodHistory)
                {
                    NodeID myId = nodeIDs[3];
                    for (auto &l : history)
                    {
                        unsigned int unlSize = UNL.size();
                        for (unsigned int i = 0; i < unlSize; ++i)
                        {
                            RCLValidation v(createSTVal(env, l, nodeIDs[i]));
                            validations.add(nodeIDs[i], v);
                        }
                    }
                    NegativeUNLVote vote(myId, validations, env.journal);
                    hash_map<NodeID, unsigned int> scoreTable;
                    BEAST_EXPECT(vote.buildScoreTable(history.back(), UNL, scoreTable));
                    for(auto & s : scoreTable)
                    {
                        BEAST_EXPECT(s.second == FLAG_LEDGER);
                    }
                    firstRound = history.back();
                }
            }
            {
                LedgerHistory history;
                bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                       0, false, false, FLAG_LEDGER + 2);
                BEAST_EXPECT(goodHistory);
                if(goodHistory)
                {
                    NodeID myId = nodeIDs[3];
                    for (auto &l : history)
                    {
                        RCLValidation v(createSTVal(env, l, myId));
                        validations.add(myId, v);
                    }
                    NegativeUNLVote vote(myId, validations, env.journal);
                    hash_map<NodeID, unsigned int> scoreTable;
                    BEAST_EXPECT(!vote.buildScoreTable(history.back(), UNL, scoreTable));
                    scoreTable.clear();
                    BEAST_EXPECT(vote.buildScoreTable(firstRound, UNL, scoreTable));
                    for(auto & s : scoreTable)
                    {
                        BEAST_EXPECT(s.second == FLAG_LEDGER);
                    }
                }
            }
        }
        {
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations & validations = env.app().getValidations();

            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(10, nodeIDs, UNL);
            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false, FLAG_LEDGER + 2);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                NodeID myId(0xdeadbeef);
                for (auto &l : history)
                {
                    unsigned int unlSize = UNL.size();
                    for (unsigned int i = 0; i < unlSize; ++i)
                    {
                        RCLValidation v(createSTVal(env, l, nodeIDs[i]));
                        validations.add(nodeIDs[i], v);
                    }
                }
                NegativeUNLVote vote(myId, validations, env.journal);
                hash_map<NodeID, unsigned int> scoreTable;
                BEAST_EXPECT(!vote.buildScoreTable(history.back(), UNL, scoreTable));
            }
        }
    }

    void
    testBuildScoreTableCombination()
    {
        /*
         * local node good history, correct scores:
         * == combination:
         * -- unl size: 10, 34, 35, 80
         * -- score pattern: all 0, all 50%, all 100%, two 0% two 50% rest 100%
         */
        std::array<uint, 4> unlSizes({10, 34, 35, 80});
        std::array<std::array<uint, 3>, 4> scorePattern
                = {0, 0, 0,
                   50, 50, 50,
                   100, 100, 100,
                   0, 50, 100};

        for(uint us = 0; us < 4; ++us)
        {
            for(uint sp = 0; sp < 4; ++sp)
            {
                //std::cout << scorePattern[i][j] << std::endl;

                jtx::Env env(*this);
                //env.app().logs().threshold(beast::severities::kAll);
                RCLValidations & validations = env.app().getValidations();

                std::vector<NodeID> nodeIDs;
                hash_set<NodeID> UNL;
                createNodeIDs(unlSizes[us], nodeIDs, UNL);

                LedgerHistory history;
                bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                       0, false, false, FLAG_LEDGER);
                BEAST_EXPECT(goodHistory);
                if(goodHistory)
                {
                    NodeID myId = nodeIDs[3];//Note 3
                    uint unlSize = UNL.size();
                    for (auto &l : history)
                    {
                        uint i = 0; //looping unl
                        auto add_v =[&](uint k)
                        {
                            if((scorePattern[sp][k] == 50 && l->seq()%2==0) ||
                                scorePattern[sp][k] == 100 ||
                                nodeIDs[i] == myId)
                            {
                                RCLValidation v(createSTVal(env, l, nodeIDs[i]));
                                validations.add(nodeIDs[i], v);
                            }
                        };
                        for (; i < 2; ++i)
                        {
                            add_v(0);
                        }
                        for (; i < 4; ++i)
                        {
                            add_v(1);
                        }
                        for (; i < unlSize; ++i)
                        {
                            add_v(2);
                        }
                    }
                    NegativeUNLVote vote(myId, validations, env.journal);
                    hash_map<NodeID, uint> scoreTable;
                    BEAST_EXPECT(vote.buildScoreTable(history.back(), UNL, scoreTable));
                    uint i = 0; //looping unl
                    auto checkScores = [&](uint score, uint k) -> bool
                    {
                        if (nodeIDs[i] == myId)
                            return score == FLAG_LEDGER;
                        if (scorePattern[sp][k] == 0)
                            return score == 0;
                        if (scorePattern[sp][k] == 50)
                            return score == FLAG_LEDGER / 2;
                        if (scorePattern[sp][k] == 100)
                            return score == FLAG_LEDGER;
                        else
                            assert(0);
                    };
                    for (; i < 2; ++i)
                    {
                        BEAST_EXPECT(checkScores(scoreTable[nodeIDs[i]], 0));
                    }
                    for (; i < 4; ++i)
                    {
                        BEAST_EXPECT(checkScores(scoreTable[nodeIDs[i]], 1));
                    }
                    for (; i < unlSize; ++i)
                    {
                        BEAST_EXPECT(checkScores(scoreTable[nodeIDs[i]], 2));
                    }
                }
            }
        }
    }

    void
    testFindAllCandidatesCombination()
    {
        /*
         * == combination 1:
         * -- unl size: 34, 35, 80
         * -- nUnl size: 0, 50%, all
         * -- score pattern: all 0, all nUnlLowWaterMark & +1 & -1, all nUnlHighWaterMark & +1 & -1, all 100%
         *
         * == combination 2:
         * -- unl size: 34, 35, 80
         * -- nUnl size: 0, all
         * -- nUnl size: one on one off one on off,
         * -- score pattern: 2*(nUnlLowWaterMark, +1, -1) & 2*(nUnlHighWaterMark, +1, -1) & rest nUnlMinLocalValsToVote
         */

        jtx::Env env(*this);
        //env.app().logs().threshold(beast::severities::kAll);
        RCLValidations & validations = env.app().getValidations();
        NodeID myId(0xA0);
        NegativeUNLVote vote(myId, validations, env.journal);

        std::array<uint, 3> unlSizes({34, 35, 80});
        std::array<uint, 3> nUnlPercent({0, 50, 100});
        std::array<uint, 8> scores({0,
                                    NegativeUNLVote::nUnlLowWaterMark - 1,
                                    NegativeUNLVote::nUnlLowWaterMark,
                                    NegativeUNLVote::nUnlLowWaterMark + 1,
                                    NegativeUNLVote::nUnlHighWaterMark - 1,
                                    NegativeUNLVote::nUnlHighWaterMark,
                                    NegativeUNLVote::nUnlHighWaterMark + 1,
                                    NegativeUNLVote::nUnlMinLocalValsToVote});

        //== combination 1:
        {
            auto fillScoreTable = [&](uint unl_size, uint nUnl_size, uint score,
                                      hash_set<NodeID> & UNL,
                                      hash_set<NodeID> & nUnl,
                                      hash_map<NodeID, unsigned int> & scoreTable)
            {
                std::vector<NodeID> nodeIDs;
                createNodeIDs(unl_size, nodeIDs, UNL);
                for(auto & n : UNL)
                    scoreTable[n] = score;
                for(uint i = 0; i < nUnl_size; ++i)
                    nUnl.insert(nodeIDs[i]);
            };

            for(auto us : unlSizes)
            {
                for (auto np : nUnlPercent)
                {
                    for (auto score : scores)
                    {
                        hash_set<NodeID> UNL;
                        hash_set<NodeID> nUnl;
                        hash_map<NodeID, unsigned int> scoreTable;

                        fillScoreTable(us, us*np/100, score, UNL, nUnl, scoreTable);
                        BEAST_EXPECT(UNL.size() == us);
                        BEAST_EXPECT(nUnl.size() == us*np/100);
                        BEAST_EXPECT(scoreTable.size() == us);
                        std::vector<NodeID> addCandidates;
                        std::vector<NodeID> removeCandidates;
                        vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);

                        if(np == 0)
                        {
                            if(score < NegativeUNLVote::nUnlLowWaterMark)
                            {
                                BEAST_EXPECT(addCandidates.size() == us);
                            }
                            else
                            {
                                BEAST_EXPECT(addCandidates.size() == 0);
                            }
                            BEAST_EXPECT(removeCandidates.size() == 0);
                        }
                        else if(np == 50)
                        {
                            BEAST_EXPECT(addCandidates.size() == 0);
                            if(score > NegativeUNLVote::nUnlHighWaterMark)
                            {
                                BEAST_EXPECT(removeCandidates.size() == us*np/100);
                            }
                            else
                            {
                                BEAST_EXPECT(removeCandidates.size() == 0);
                            }
                        }
                        else
                        {
                            BEAST_EXPECT(addCandidates.size() == 0);
                            if(score > NegativeUNLVote::nUnlHighWaterMark)
                            {
                                BEAST_EXPECT(removeCandidates.size() == us);
                            }
                            else
                            {
                                BEAST_EXPECT(removeCandidates.size() == 0);
                            }
                        }
                    }
                }
            }
        }

        //== combination 2:
        {
            auto fillScoreTable = [&](uint unl_size, uint nUnl_percent,
                                      hash_set<NodeID> & UNL,
                                      hash_set<NodeID> & nUnl,
                                      hash_map<NodeID, unsigned int> & scoreTable)
            {
                std::vector<NodeID> nodeIDs;
                createNodeIDs(unl_size, nodeIDs, UNL);
                uint nIdx = 0;
                for(auto score : scores)
                {
                    scoreTable[nodeIDs[nIdx++]] = score;
                    scoreTable[nodeIDs[nIdx++]] = score;
                }
                for(; nIdx < unl_size; )
                {
                    scoreTable[nodeIDs[nIdx++]] = scores.back();
                }

                if(nUnl_percent == 100)
                {
                    nUnl = UNL;
                }
                else if(nUnl_percent == 50)
                {
                    for(uint i = 1; i < unl_size; i += 2)
                        nUnl.insert(nodeIDs[i]);
                }
            };

            for(auto us : unlSizes)
            {
                for (auto np : nUnlPercent)
                {
                    hash_set<NodeID> UNL;
                    hash_set<NodeID> nUnl;
                    hash_map<NodeID, unsigned int> scoreTable;

                    fillScoreTable(us, np, UNL, nUnl, scoreTable);
                    BEAST_EXPECT(UNL.size() == us);
                    BEAST_EXPECT(nUnl.size() == us*np/100);
                    BEAST_EXPECT(scoreTable.size() == us);
                    std::vector<NodeID> addCandidates;
                    std::vector<NodeID> removeCandidates;
                    vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);

                    if(np == 0)
                    {
                        BEAST_EXPECT(addCandidates.size() == 4);
                        BEAST_EXPECT(removeCandidates.size() == 0);
                    }
                    else if(np == 50)
                    {
                        BEAST_EXPECT(addCandidates.size() == 0);//already have maxNegativeListed
                        BEAST_EXPECT(removeCandidates.size() == nUnl.size() - 6);
                    }
                    else
                    {
                        BEAST_EXPECT(addCandidates.size() == 0);
                        BEAST_EXPECT(removeCandidates.size() == nUnl.size() - 12);
                    }
                }
            }
        }
    }

    void
    testFindAllCandidates()
    {
        /*
         * -- unl size: 35
         * -- nUnl size: 3
         *
         * 0. all good scores
         * 1. all bad scores
         * 2. all between watermarks
         * 3. 2 good scorers in nUnl
         * 4. 2 bad scorers not in nUnl
         * 5. 2 in nUnl but not in unl, have a remove candidate from score table
         * 6. 2 in nUnl but not in unl, no remove candidate from score table
         * 7. 2 new validators have good scores, already in nUnl
         * 8. 2 new validators have bad scores, not in nUnl
         * 9. expired the new validators have bad scores, not in nUnl
         */

        jtx::Env env(*this);
        //env.app().logs().threshold(beast::severities::kAll);
        RCLValidations & validations = env.app().getValidations();
        std::vector<NodeID> nodeIDs;
        hash_set<NodeID> UNL;
        createNodeIDs(35, nodeIDs, UNL);
        hash_set<NodeID> nUnl;
        for(uint i = 0; i < 3; ++i)
            nUnl.insert(nodeIDs[i]);
        hash_map<NodeID, unsigned int> goodScoreTable;
        for(auto & n : nodeIDs)
            goodScoreTable[n] = NegativeUNLVote::nUnlHighWaterMark + 1;
        NodeID myId = nodeIDs[0];
        NegativeUNLVote vote(myId, validations, env.journal);

        {
            //all good scores
            hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 0);
            BEAST_EXPECT(removeCandidates.size() == 3);
        }
        {
            //all bad scores
            hash_map<NodeID, unsigned int> scoreTable;
            for(auto & n : nodeIDs)
                scoreTable[n] = NegativeUNLVote::nUnlLowWaterMark - 1;
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 35 - 3);
            BEAST_EXPECT(removeCandidates.size() == 0);
        }
        {
            //all between watermarks
            hash_map<NodeID, unsigned int> scoreTable;
            for(auto & n : nodeIDs)
                scoreTable[n] = NegativeUNLVote::nUnlLowWaterMark + 1;
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 0);
            BEAST_EXPECT(removeCandidates.size() == 0);
        }

        {
            //2 good scorers in nUnl
            hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
            scoreTable[nodeIDs[2]] = NegativeUNLVote::nUnlLowWaterMark + 1;
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 0);
            BEAST_EXPECT(removeCandidates.size() == 2);
        }

        {
            //2 bad scorers not in nUnl
            hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
            scoreTable[nodeIDs[11]] = NegativeUNLVote::nUnlLowWaterMark - 1;
            scoreTable[nodeIDs[12]] = NegativeUNLVote::nUnlLowWaterMark - 1;
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 2);
            BEAST_EXPECT(removeCandidates.size() == 3);
        }

        {
            //2 in nUnl but not in unl, have a remove candidate from score table
            hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
            hash_set<NodeID> UNL_temp = UNL;
            UNL_temp.erase(nodeIDs[0]);
            UNL_temp.erase(nodeIDs[1]);
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL_temp, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 0);
            BEAST_EXPECT(removeCandidates.size() == 3);
        }

        {
            //2 in nUnl but not in unl, no remove candidate from score table
            hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
            scoreTable.erase(nodeIDs[0]);
            scoreTable.erase(nodeIDs[1]);
            scoreTable[nodeIDs[2]] = NegativeUNLVote::nUnlLowWaterMark + 1;
            hash_set<NodeID> UNL_temp = UNL;
            UNL_temp.erase(nodeIDs[0]);
            UNL_temp.erase(nodeIDs[1]);
            std::vector<NodeID> addCandidates;
            std::vector<NodeID> removeCandidates;
            vote.findAllCandidates(UNL_temp, nUnl, scoreTable, addCandidates, removeCandidates);
            BEAST_EXPECT(addCandidates.size() == 0);
            BEAST_EXPECT(removeCandidates.size() == 2);
        }

        {
            //2 new validators
            NodeID new_1(0xbead);
            NodeID new_2(0xbeef);
            hash_set<NodeID> nowTrusted = {new_1, new_2};
            hash_set<NodeID> UNL_temp = UNL;
            UNL_temp.insert(new_1);
            UNL_temp.insert(new_2);
            vote.newValidators(256, nowTrusted);
            {
                //2 new validators have good scores, already in nUnl
                hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
                scoreTable[new_1] = NegativeUNLVote::nUnlHighWaterMark + 1;
                scoreTable[new_2] = NegativeUNLVote::nUnlHighWaterMark + 1;
                hash_set<NodeID> nUnl_temp = nUnl;
                nUnl_temp.insert(new_1);
                nUnl_temp.insert(new_2);
                std::vector<NodeID> addCandidates;
                std::vector<NodeID> removeCandidates;
                vote.findAllCandidates(UNL_temp, nUnl_temp, scoreTable, addCandidates, removeCandidates);
                BEAST_EXPECT(addCandidates.size() == 0);
                BEAST_EXPECT(removeCandidates.size() == 3+2);
            }
            {
                //2 new validators have bad scores, not in nUnl
                hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
                scoreTable[new_1] = 0;
                scoreTable[new_2] = 0;
                std::vector<NodeID> addCandidates;
                std::vector<NodeID> removeCandidates;
                vote.findAllCandidates(UNL_temp, nUnl, scoreTable, addCandidates, removeCandidates);
                BEAST_EXPECT(addCandidates.size() == 0);
                BEAST_EXPECT(removeCandidates.size() == 3);
            }
            {
                //expired the new validators have bad scores, not in nUnl
                vote.purgeNewValidators(256 + NegativeUNLVote::newValidatorMeasureSkip + 1);
                hash_map<NodeID, unsigned int> scoreTable = goodScoreTable;
                scoreTable[new_1] = 0;
                scoreTable[new_2] = 0;
                std::vector<NodeID> addCandidates;
                std::vector<NodeID> removeCandidates;
                vote.findAllCandidates(UNL_temp, nUnl, scoreTable, addCandidates, removeCandidates);
                BEAST_EXPECT(addCandidates.size() == 2);
                BEAST_EXPECT(removeCandidates.size() == 3);
            }
        }
    }

    void
    testNewValidators()
    {
        jtx::Env env(*this);
        //env.app().logs().threshold(beast::severities::kAll);
        NodeID myId(0xA0);
        RCLValidations & validations = env.app().getValidations();
        NegativeUNLVote vote(myId, validations, env.journal);

        //empty, add
        //three, add new, add same
        //empty, purge
        //three, 0, 1, 2, 3 expired

        NodeID n1(0xA1);
        NodeID n2(0xA2);
        NodeID n3(0xA3);

        vote.newValidators(2,{n1});
        BEAST_EXPECT(vote.newValidators_.size() == 1);
        if(vote.newValidators_.size() == 1)
        {
            BEAST_EXPECT(vote.newValidators_.begin()->first == n1);
            BEAST_EXPECT(vote.newValidators_.begin()->second == 2);
        }

        vote.newValidators(3,{n1, n2});
        BEAST_EXPECT(vote.newValidators_.size() == 2);
        if(vote.newValidators_.size() == 2)
        {
            BEAST_EXPECT(vote.newValidators_[n1] == 2);
            BEAST_EXPECT(vote.newValidators_[n2] == 3);
        }

        vote.newValidators(NegativeUNLVote::newValidatorMeasureSkip,{n1, n2, n3});
        BEAST_EXPECT(vote.newValidators_.size() == 3);
        if(vote.newValidators_.size() == 3)
        {
            BEAST_EXPECT(vote.newValidators_[n1] == 2);
            BEAST_EXPECT(vote.newValidators_[n2] == 3);
            BEAST_EXPECT(vote.newValidators_[n3] == NegativeUNLVote::newValidatorMeasureSkip);
        }

        vote.purgeNewValidators(NegativeUNLVote::newValidatorMeasureSkip+2);
        BEAST_EXPECT(vote.newValidators_.size() == 3);
        vote.purgeNewValidators(NegativeUNLVote::newValidatorMeasureSkip+3);
        BEAST_EXPECT(vote.newValidators_.size() == 2);
        vote.purgeNewValidators(NegativeUNLVote::newValidatorMeasureSkip+4);
        BEAST_EXPECT(vote.newValidators_.size() == 1);
        BEAST_EXPECT(vote.newValidators_.begin()->first == n3);
        BEAST_EXPECT(vote.newValidators_.begin()->second == NegativeUNLVote::newValidatorMeasureSkip);
    }

    void
    testDoVoting()
    {
        /*
         * == use hasToAdd and hasToRemove in some of the cases
         *
         * == all good score, nUnl empty
         * -- txSet.size = 0
         * == all good score, nUnl not empty (use hasToAdd)
         * -- txSet.size = 1
         *
         * == 2 nodes offline, nUnl empty (use hasToRemove)
         * -- txSet.size = 1
         * == 2 nodes offline, in nUnl
         * -- txSet.size = 0
         * == 2 nodes offline, not in nUnl, but maxListed
         * -- txSet.size = 0
         * == 2 nodes offline including me, not in nUnl
         * -- txSet.size = 0
         * == 2 nodes offline, not in nUnl, but I'm not a validator
         * -- txSet.size = 0
         *
         * == 2 in nUnl, but not in unl, no other remove candidates
         * -- txSet.size = 1
         *
         * == 2 new validators have bad scores
         * -- txSet.size = 0
         * == 2 expired new validators have bad scores
         * -- txSet.size = 1
         */

        {
            //== all good score, nUnl empty
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(47, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false);
            BEAST_EXPECT(goodHistory);
            if (goodHistory)
            {
                for (auto &l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs[0], validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 0);
            }
        }

        {
            //all good score, nUnl not empty (use hasToAdd)
            //-- txSet.size = 1
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(51, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, true, false);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs[0], validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 1);
            }
        }

        {
            //== 2 nodes offline, nUnl empty (use hasToRemove)
            //-- txSet.size = 1
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(39, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   1, false, true);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        if(n == nodeIDs[0] || n == nodeIDs[1])
                            continue;
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs.back(), validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 1);
            }
        }

        {
            //2 nodes offline, in nUnl
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(30, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   1, true, false);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        if(n == nodeIDs[0] || n == nodeIDs[1])
                            continue;
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs.back(), validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 0);
            }
        }

        {
            //2 nodes offline, not in nUnl, but maxListed
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(32, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                    8, true, true);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (uint i = 11; i < 32; ++i)
                    {
                        RCLValidation v(createSTVal(env, l, nodeIDs[i]));
                        validations.add(nodeIDs[i], v);
                    }
                }
                NegativeUNLVote vote(nodeIDs.back(), validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 0);
            }
        }

        {
            //== 2 nodes offline including me, not in nUnl
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(33, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        if(n == nodeIDs[0] || n == nodeIDs[1])
                            continue;
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs[0], validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 0);
            }
        }

        {
            //2 nodes offline, not in nUnl, but I'm not a validator
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(40, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        if(n == nodeIDs[0] || n == nodeIDs[1])
                            continue;
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(NodeID(0xdeadbeef), validations, env.journal);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 0);
            }
        }


        {
            //== 2 in nUnl, but not in unl, no other remove candidates
            //-- txSet.size = 1
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(25, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   2, false, false);
            BEAST_EXPECT(goodHistory);
            if(goodHistory)
            {
                for(auto & l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        if(n == nodeIDs[0] || n == nodeIDs[1])
                            continue;
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs.back(), validations, env.journal);
                UNL.erase(nodeIDs[0]);
                UNL.erase(nodeIDs[1]);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 1);
            }
        }


        {
            //== 2 new validators have bad scores
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(15, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                                                   0, false, false);
            BEAST_EXPECT(goodHistory);
            if (goodHistory)
            {
                for (auto &l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs[0], validations, env.journal);
                hash_set<NodeID> nowTrusted;
                nowTrusted.emplace(0xdead);
                nowTrusted.emplace(0xbeef);
                UNL.insert(nowTrusted.begin(), nowTrusted.end());
                vote.newValidators(history.back()->seq(), nowTrusted);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 0);
            }
        }

        {
            //== 2 expired new validators have bad scores
            //-- txSet.size = 0
            jtx::Env env(*this);
            //env.app().logs().threshold(beast::severities::kAll);
            RCLValidations &validations = env.app().getValidations();
            std::vector<NodeID> nodeIDs;
            hash_set<NodeID> UNL;
            createNodeIDs(21, nodeIDs, UNL);

            LedgerHistory history;
            bool goodHistory = createLedgerHistory(history, env, nodeIDs,
                    0, false, false,
                    NegativeUNLVote::newValidatorMeasureSkip * 2);
            BEAST_EXPECT(goodHistory);
            if (goodHistory)
            {
                for (auto &l : history)
                {
                    for (auto &n : nodeIDs)
                    {
                        RCLValidation v(createSTVal(env, l, n));
                        validations.add(n, v);
                    }
                }
                NegativeUNLVote vote(nodeIDs[0], validations, env.journal);
                hash_set<NodeID> nowTrusted;
                nowTrusted.emplace(0xdead);
                nowTrusted.emplace(0xbeef);
                UNL.insert(nowTrusted.begin(), nowTrusted.end());
                vote.newValidators(FLAG_LEDGER, nowTrusted);
                auto txSet = std::make_shared<SHAMap>(
                        SHAMapType::TRANSACTION, env.app().family());
                vote.doVoting(history.back(), UNL, txSet);
                BEAST_EXPECT(countTx(txSet) == 1);
            }
        }
    }

    void run() override
    {
        testAddTx();
        testPickOneCandidate();
        testBuildScoreTable();
        testBuildScoreTableCombination();
        testFindAllCandidates();
        testFindAllCandidatesCombination();
        testNewValidators();
        testDoVoting();
    }
};

BEAST_DEFINE_TESTSUITE(NegativeUNLVote,consensus,ripple);

}  // test
}  // ripple
