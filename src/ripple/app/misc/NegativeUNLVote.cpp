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
//#include <ripple/beast/utility/Journal.h>

namespace ripple {

NegativeUNLVote::NegativeUNLVote(NodeID const& myId,
    RCLValidations & validations,
    beast::Journal j)
    : myId_(myId)
    , validations_(validations)
    , j_(j)
{}

void
NegativeUNLVote::doVoting (LedgerConstPtr & prevLedger,
          hash_set<NodeID> const & unl,
          std::shared_ptr<SHAMap> const& initialSet)
{
    auto const seq = prevLedger->info().seq + 1;
    hash_map<NodeID, unsigned int> scoreTable;
    if(buildScoreTable(prevLedger, unl, scoreTable))
    {
        hash_set<NodeID> nextNegativeUNL = prevLedger->negativeUNL();
        auto negativeUNLToAdd = prevLedger->negativeUNLToAdd();
        auto negativeUNLToRemove = prevLedger->negativeUNLToRemove();
        if (negativeUNLToAdd)
            nextNegativeUNL.insert(*negativeUNLToAdd);
        if (negativeUNLToRemove)
            nextNegativeUNL.erase(*negativeUNLToRemove);

        purgeNewValidators(seq);

        std::vector<NodeID> addCandidates;
        std::vector<NodeID> removeCandidates;
        findAllCandidates(unl, nextNegativeUNL, scoreTable,
            addCandidates,removeCandidates);

        if (!addCandidates.empty())
        {
            JLOG(j_.debug()) << "N-UNL: addCandidates.size "//TODO remove log line
                             << addCandidates.size();
            addTx(seq, pickOneCandidate(prevLedger->info().hash, addCandidates), true, initialSet);
        }

        if (!removeCandidates.empty())
        {
            JLOG(j_.debug()) << "N-UNL: removeCandidates in UNL, size "//TODO remove log line
                             << removeCandidates.size();
            addTx(seq, pickOneCandidate(prevLedger->info().hash, removeCandidates), false, initialSet);
        }
    }
}

void
NegativeUNLVote::addTx(LedgerIndex seq,
        NodeID const &nid,
        bool adding,
        std::shared_ptr<SHAMap> const& initialSet)
{
//    JLOG(j_.trace()) << "N-UNL: ledger " << seq;
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
        JLOG(j_.warn()) << "N-UNL: ledger " << seq
                        << " add tx failed";
    }
    else
    {
        JLOG(j_.debug()) << "N-UNL: ledger " << seq
                         << " add Tx with txID: " << txID;
    }
}

NodeID
NegativeUNLVote::pickOneCandidate(uint256 randomPadData,
        std::vector<NodeID> & candidates)
{
    assert(!candidates.empty());
    static_assert(NodeID::bytes <= uint256::bytes);
    NodeID randomPad = NodeID::fromVoid(randomPadData.data());
    NodeID txNodeID = candidates[0];
    for (int j = 1; j < candidates.size(); ++j)
    {
        //TODO remove log line
        JLOG(j_.trace()) << "N-UNL:"// ledger " << seq
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
    JLOG(j_.debug()) << "N-UNL:"// ledger " << seq
                          << " picked candidate " << txNodeID;
    return txNodeID;
}

bool
NegativeUNLVote::buildScoreTable(LedgerConstPtr & prevLedger,
        hash_set<NodeID> const & unl,
        hash_map<NodeID, unsigned int> & scoreTable)
{
    assert(scoreTable.empty());
    auto const hashIndex = prevLedger->read(keylet::skip());
    if (!hashIndex)
        return false;
    auto const seq = prevLedger->info().seq + 1; // TODO delete
    auto ledgerAncestors = hashIndex->getFieldV256(sfHashes).value();
    auto numAncestors = ledgerAncestors.size();
    if(numAncestors < FLAG_LEDGER)
    {
        JLOG(j_.debug()) << "N-UNL: ledger " << seq
                              << " not enough history. Can trace back only "
                              << numAncestors << " ledgers.";
        return false;
    }

    // have enough ledger ancestors
    for (auto & k : unl)
    {
        scoreTable[k] = 0;
    }
    auto idx = numAncestors - 1;
    for(int i = 0; i < FLAG_LEDGER; ++i)
    {
        for (auto const& v :
                validations_.getTrustedForLedger(ledgerAncestors[idx--]))
        {
            if(scoreTable.find(v->getNodeID()) != scoreTable.end())
                    ++scoreTable[v->getNodeID()];
        }
    }

    unsigned int myValidationCount = 0;
    if(scoreTable.find(myId_) != scoreTable.end())
        myValidationCount = scoreTable[myId_];
    if(myValidationCount < nUnlMinLocalValsToVote)
    {
        JLOG(j_.debug()) << "N-UNL: ledger " << seq
                              << ". I only issued " << myValidationCount
                              << " validations in last " << FLAG_LEDGER
                              << " ledgers."
                              << " My reliability measurement could be wrong.";
        return false;
    }
    else if(myValidationCount >= nUnlMinLocalValsToVote
            && myValidationCount <= FLAG_LEDGER)
    {
        return true;
    }
    else
    {
        //cannot happen unless validations_.getTrustedForLedger returns
        //multiple validations from the same validator.
        JLOG(j_.error()) << "N-UNL: ledger " << seq
                              << ". I issued " << myValidationCount
                              << " validations in last " << FLAG_LEDGER
                              << " ledgers. I issued too many.";
        return false;
    }
}

void
NegativeUNLVote::findAllCandidates(hash_set<NodeID> const& unl,
        hash_set<NodeID> const& nextNUnl,
        hash_map<NodeID, unsigned int> const& scoreTable,
        std::vector<NodeID> & addCandidates,
        std::vector<NodeID> & removeCandidates)
{
    auto maxNegativeListed = (std::size_t) std::ceil(unl.size() * nUnlMaxListed);
    std::size_t negativeListed = 0;
    for (auto const &n : unl)
    {
        if (nextNUnl.find(n) != nextNUnl.end())
            ++negativeListed;
    }
    bool canAdd = maxNegativeListed > negativeListed;
    JLOG(j_.trace()) << "N-UNL:"// ledger " << seq //TODO delete
                          << " my nodeId " << myId_
                          << " lowWaterMark " << nUnlLowWaterMark
                          << " highWaterMark " << nUnlHighWaterMark
                          << " canAdd " << canAdd
                          << " maxNegativeListed " << maxNegativeListed
                          << " negativeListed " << negativeListed;

    for (auto it = scoreTable.cbegin(); it != scoreTable.cend(); ++it)
    {
        JLOG(j_.debug()) << "N-UNL:"// ledger " << seq //TODO delete
                         << " node " << it->first
                         << " score " << it->second;

        if (canAdd &&
            it->second < nUnlLowWaterMark &&
            nextNUnl.find(it->first) == nextNUnl.end() &&
            newValidators_.find(it->first) == newValidators_.end())
        {
            JLOG(j_.debug()) << "N-UNL:"// ledger " << seq //TODO delete
                             << " addCandidates.push_back " << it->first;
            addCandidates.push_back(it->first);
        }

        if (it->second > nUnlHighWaterMark &&
            nextNUnl.find(it->first) != nextNUnl.end())
        {
            JLOG(j_.debug()) << "N-UNL:"// ledger " << seq //TODO delete
                             << " removeCandidates.push_back " << it->first;
            removeCandidates.push_back(it->first);
        }
    }

    if (removeCandidates.empty())
    {
        for (auto &n : nextNUnl)
        {
            if (unl.find(n) == unl.end())
            {
                removeCandidates.push_back(n);
            }
        }
    }
}

void
NegativeUNLVote::newValidators (LedgerIndex seq,
        hash_set<NodeID> const& nowTrusted)
{
    std::lock_guard lock(mutex_);
    for(auto & n : nowTrusted)
    {
        if(newValidators_.find(n) == newValidators_.end())
        {
            newValidators_[n] = seq;
        }
    }
}

void
NegativeUNLVote::purgeNewValidators(LedgerIndex seq)
{
    std::lock_guard lock(mutex_);
    auto i = newValidators_.begin();
    while(i != newValidators_.end())
    {
        if(seq - i->second > newValidatorMeasureSkip)
        {
            i = newValidators_.erase(i);
        }
        else{
            ++i;
        }
    }
}

} // ripple



