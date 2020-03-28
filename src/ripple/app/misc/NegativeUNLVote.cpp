//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/misc/NegativeUNLVote.h>
#include <ripple/beast/utility/Journal.h>

namespace ripple {

void
doNegativeUNLVoting (NodeID const & myId,
          std::shared_ptr<Ledger const> const& prevLedger,
          hash_set<NodeID> unl,
          RCLValidations & validations,
          std::shared_ptr<SHAMap> const& initialSet,
          beast::Journal const & journal)
{
    auto const hashIndex = prevLedger->read(keylet::skip());
    if (!hashIndex )
        return;
    auto const seq = prevLedger->info().seq + 1;
    auto ledgerAncestors = hashIndex->getFieldV256(sfHashes).value();
    auto numAncestors = ledgerAncestors.size();
    if(numAncestors < FLAG_LEDGER)
    {
        JLOG(journal.debug()) << "N-UNL: ledger " << seq
                              << " not enough history. Can trace back only "
                              << numAncestors << " ledgers.";
        return;
    }

    // have enough ledger ancestors
    hash_map<NodeID, unsigned int> scoreTable;
    for (auto & k : unl)
    {
        if(k != myId)
            scoreTable[k] = 0;
    }
    auto idx = numAncestors - 1;
    unsigned int myValidationCount = 0;
    for(int i = 0; i < FLAG_LEDGER; ++i)
    {
        for (auto const& v :
                validations.getTrustedForLedger(ledgerAncestors[idx--]))
        {
            if(v->getNodeID() == myId)
            {
                ++myValidationCount;
            }
            else
            {
                if(scoreTable.find(v->getNodeID()) != scoreTable.end())
                    ++scoreTable[v->getNodeID()];
            }
        }
    }

    if(myValidationCount < nUnlMinLocalValsToVote)
    {
        JLOG(journal.debug()) << "N-UNL: ledger " << seq
                              << ". I only issued " << myValidationCount
                              << " validations in last " << FLAG_LEDGER
                              << " ledgers."
                              << " My reliability measurement could be wrong.";
        return;
    }
    else if(myValidationCount >= nUnlMinLocalValsToVote
            && myValidationCount <= FLAG_LEDGER)
    {
        hash_set<NodeID> nextNegativeUNL = prevLedger->negativeUNL();
        auto negativeUNLToAdd = prevLedger->negativeUNLToAdd();
        auto negativeUNLToRemove = prevLedger->negativeUNLToRemove();
        if (negativeUNLToAdd)
            nextNegativeUNL.insert(*negativeUNLToAdd);
        if (negativeUNLToRemove)
            nextNegativeUNL.erase(*negativeUNLToRemove);

        auto maxNegativeListed = (std::size_t) std::ceil(unl.size() * nUnlMaxListed);
        std::size_t negativeListed = 0;
        for (auto const &n : unl)
        {
            if (nextNegativeUNL.find(n) != nextNegativeUNL.end())
                ++negativeListed;
        }
        bool canAdd = maxNegativeListed > negativeListed;
        JLOG(journal.trace()) << "N-UNL: ledger " << seq
                              << " my nodeId " << myId
                              << " lowWaterMark " << nUnlLowWaterMark
                              << " highWaterMark " << nUnlHighWaterMark
                              << " canAdd " << canAdd
                              << " maxNegativeListed " << maxNegativeListed
                              << " negativeListed " << negativeListed;

        std::vector<NodeID> addCandidates;
        std::vector<NodeID> removeCandidates;
        for (auto it = scoreTable.cbegin(); it != scoreTable.cend(); ++it)
        {
            JLOG(journal.debug()) << "N-UNL: ledger " << seq //TODO delete
                             << " node " << it->first
                             << " score " << it->second;

            if (canAdd && it->second < nUnlLowWaterMark &&
                nextNegativeUNL.find(it->first) == nextNegativeUNL.end())
            {
                JLOG(journal.debug()) << "N-UNL: ledger " << seq //TODO delete
                                 << " addCandidates.push_back " << it->first;
                addCandidates.push_back(it->first);
            }
            if (it->second > nUnlHighWaterMark &&
                nextNegativeUNL.find(it->first) != nextNegativeUNL.end())
            {
                JLOG(journal.debug()) << "N-UNL: ledger " << seq //TODO delete
                                 << " removeCandidates.push_back " << it->first;
                removeCandidates.push_back(it->first);
            }
        }

        auto addTx = [&](NodeID const &nid, bool adding)
        {
            STTx nunlTx(ttNEGATIVE_UNL,
                        [&](auto &obj)
                        {
                            obj.setFieldU8(sfNegativeUNLTxAdd, adding ? 1 : 0);
                            obj.setFieldU32(sfLedgerSequence, seq);
                            obj.setFieldH160(sfNegativeUNLTxNodeID, nid);
                        });

            uint256 txID = nunlTx.getTransactionID();
            Serializer s;
            nunlTx.add(s);
            auto tItem = std::make_shared<SHAMapItem>(txID, s.peekData());
            if (!initialSet->addGiveItem(tItem, true, false))
            {
                JLOG(journal.warn()) << "N-UNL: ledger " << seq
                                     << " add tx failed";
            }
            else
            {
                JLOG(journal.trace()) << "N-UNL: ledger " << seq
                                     << " add Tx with txID: " << txID;
            }
        };

        uint256 randomPadData = prevLedger->info().hash;
        static_assert(NodeID::bytes <= uint256::bytes);
        NodeID randomPad = NodeID::fromVoid(randomPadData.data());

        auto findAndAddTx = [&](bool adding)
        {
            auto &candidates = adding ? addCandidates : removeCandidates;
            NodeID txNodeID = candidates[0];
            for (int j = 1; j < candidates.size(); ++j)
            {
                //TODO remove log line
                JLOG(journal.debug()) << "N-UNL: ledger " << seq
                                 << " randomPad " << randomPad
                                 << " txNodeID " << txNodeID
                                 << " candidates[j] " << candidates[j]
                                 << " txNodeID ^ randomPad " << (txNodeID ^ randomPad)
                                 << " candidates[j] ^ randomPad) " << (candidates[j] ^ randomPad);
                if ((candidates[j] ^ randomPad) < (txNodeID ^ randomPad))
                {
                    txNodeID = candidates[j];
                }
            }
            JLOG(journal.debug()) << "N-UNL: ledger " << seq
                                  << (adding ? "toAdd: " : "toRemove: ") << txNodeID;
            addTx(txNodeID, adding);
        };

        if (addCandidates.size() > 0)
        {
            JLOG(journal.debug()) << "N-UNL: addCandidates.size "//TODO remove log line
                             << addCandidates.size();
            findAndAddTx(true);
        }

        if (removeCandidates.size() > 0)
        {
            JLOG(journal.debug()) << "N-UNL: removeCandidates in UNL, size "//TODO remove log line
                             << removeCandidates.size();
            findAndAddTx(false);
        }
        else
        {
            for (auto &n : nextNegativeUNL)
            {
                if (scoreTable.find(n) == scoreTable.end())
                {
                    removeCandidates.push_back(n);
                }
            }
            if (removeCandidates.size() > 0)
            {
                JLOG(journal.debug()) << "N-UNL: removeCandidates not in UNL, size "//TODO remove log line
                             << removeCandidates.size();
                findAndAddTx(false);
            };
        }
        return;
    }
    else
    {
        JLOG(journal.error()) << "N-UNL: ledger " << seq
                              << ". I issued " << myValidationCount
                              << " validations in last " << FLAG_LEDGER
                              << " ledgers. I issued too many.";
        return;
    }
}
} // ripple

//bad validator
//                    {
//                        std::vector<NodeID> ids(unl.begin(), unl.end());
//                        std::sort(ids.begin(), ids.end());
//
//                        auto unl_i = ids.begin();
//                        addTx(*unl_i, true);
//                        ++unl_i;
//                        addTx(*unl_i, true);
//                        ++unl_i;
//                        addTx(*unl_i, true);
//                    }

    //TODO rewrite after unit test
    /*
     * Figure out the negative UNL Tx candidates
     *
     * Prepare:
     * 1. create an empty validation agreement score table,
     *    one row per validator in the UNL
     * 2. for FLAG_LEDGER number of ledgers, starting at the prevLedger:
     *    -- get the NodeIDs of validators agreed with us
     *    -- increase their score by one
     *
     * The candidate to remove from the negative UNL:
     * -- on the negative UNL (including the to-be-added one) and
     *    has the highest score, the score must >= high-water mark
     * -- if not found, then find one on the negative UNL but not
     *    on the UNL. If multiple are found, choose the one with
     *    the smallest numerical value of NodeID xor prevLedger_hash
     * -- if still not found, then no negative UNL Tx for removing
     *
     * The candidate to add to the negative UNL:
     * -- not on the negative UNL (including the to-be-added one) and
     *    has the lowest score that < low-water mark
     * -- if still not found, then no negative UNL Tx for adding
     */


