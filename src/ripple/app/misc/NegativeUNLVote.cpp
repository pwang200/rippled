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

namespace ripple {

NegativeUNLVote::NegativeUNLVote(NodeID const& myId,
    beast::Journal j)
    : myId_(myId)
    , j_(j)
{}

void
NegativeUNLVote::doVoting (LedgerConstPtr & prevLedger,
                           hash_set<PublicKey> const & keys,
                           RCLValidations & validations,
                           std::shared_ptr<SHAMap> const& initialSet)
{
    auto const seq = prevLedger->info().seq + 1;
    hash_map<NodeID, unsigned int> scoreTable;
    hash_map<NodeID, PublicKey> nidToKeyMap;
    hash_set<NodeID> unlNodeIDs;
    for(auto const &k : keys)
    {
        auto nid = calcNodeID(k);
        nidToKeyMap.emplace(nid, k);
        unlNodeIDs.emplace(nid);
    }

    if(buildScoreTable(prevLedger, unlNodeIDs, validations, scoreTable))
    {
        //build next nUnl
        auto nUnlKeys = prevLedger->nUnl();
        auto nUnlToDisable = prevLedger->nUnlToDisable();
        auto nUnlToReEnable = prevLedger->nUnlToReEnable();
        if (nUnlToDisable)
            nUnlKeys.insert(*nUnlToDisable);
        if (nUnlToReEnable)
            nUnlKeys.erase(*nUnlToReEnable);

        hash_set<NodeID> nUnlNodeIDs;
        for(auto & k : nUnlKeys)
        {
            auto nid = calcNodeID(k);
            nUnlNodeIDs.insert(nid);
            if(nidToKeyMap.find(nid) == nidToKeyMap.end())
            {
                nidToKeyMap.emplace(nid, k);
            }
        }

        purgeNewValidators(seq);

        std::vector<NodeID> toDisableCandidates;
        std::vector<NodeID> toReEnableCandidates;
        findAllCandidates(unlNodeIDs, nUnlNodeIDs, scoreTable,
                          toDisableCandidates, toReEnableCandidates);

        if (!toDisableCandidates.empty())
        {
            auto n = pickOneCandidate(prevLedger->info().hash, toDisableCandidates);
            assert(nidToKeyMap.find(n) != nidToKeyMap.end());
            addTx(seq, nidToKeyMap[n], true, initialSet);
        }

        if (!toReEnableCandidates.empty())
        {
            auto n = pickOneCandidate(prevLedger->info().hash, toReEnableCandidates);
            assert(nidToKeyMap.find(n) != nidToKeyMap.end());
            addTx(seq, nidToKeyMap[n], false, initialSet);
        }
    }
}

void
NegativeUNLVote::addTx(LedgerIndex seq,
        PublicKey const &vp,
        bool disabling,
        std::shared_ptr<SHAMap> const& initialSet)
{
    STTx nUnlTx(ttUNL_MODIDY,
                [&](auto &obj)
            {
                obj.setFieldU8(sfUNLModifyDisabling, disabling ? 1 : 0);
                obj.setFieldU32(sfLedgerSequence, seq);
                obj.setFieldVL(sfUNLModifyValidator, vp.slice());
            });

    uint256 txID = nUnlTx.getTransactionID();
    Serializer s;
    nUnlTx.add(s);
    auto tItem = std::make_shared<SHAMapItem>(txID, s.peekData());
    if (!initialSet->addGiveItem(tItem, true, false))
    {
        JLOG(j_.warn()) << "N-UNL: ledger seq=" << seq
                        << " add tx failed";
    }
    else
    {
        JLOG(j_.debug()) << "N-UNL: ledger seq=" << seq
                         << " add Tx with txID: " << txID
                         << " validator to "
                         << (disabling? "disable " : "re-enable ") << vp;
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
        if ((candidates[j] ^ randomPad) < (txNodeID ^ randomPad))
        {
            txNodeID = candidates[j];
        }
    }
    return txNodeID;
}

bool
NegativeUNLVote::buildScoreTable(LedgerConstPtr & prevLedger,
                                 hash_set<NodeID> const & unl,
                                 RCLValidations & validations,
                                 hash_map<NodeID, unsigned int> & scoreTable)
{
    assert(scoreTable.empty());
    auto const seq = prevLedger->info().seq + 1;
    validations.setSeqToKeep(seq);

    auto const hashIndex = prevLedger->read(keylet::skip());
    if (!hashIndex || !hashIndex->isFieldPresent(sfHashes))
    {
        JLOG(j_.debug()) << "N-UNL: ledger " << seq
                         << " no history.";
        return false;
    }

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
                validations.getTrustedForLedger(ledgerAncestors[idx--]))
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
        // cannot happen because validations_.getTrustedForLedger does not
        // return multiple validations of the same ledger from a validator.
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
        std::vector<NodeID> & toDisableCandidates,
        std::vector<NodeID> & toReEnableCandidates)
{
    auto maxNegativeListed = (std::size_t) std::ceil(unl.size() * nUnlMaxListed);
    std::size_t negativeListed = 0;
    for (auto const &n : unl)
    {
        if (nextNUnl.find(n) != nextNUnl.end())
            ++negativeListed;
    }
    bool canAdd = maxNegativeListed > negativeListed;
    JLOG(j_.trace()) << "N-UNL: my nodeId " << myId_
                     << " lowWaterMark " << nUnlLowWaterMark
                     << " highWaterMark " << nUnlHighWaterMark
                     << " canAdd " << canAdd
                     << " maxNegativeListed " << maxNegativeListed
                     << " negativeListed " << negativeListed;

    for (auto it = scoreTable.cbegin(); it != scoreTable.cend(); ++it)
    {
        JLOG(j_.trace()) << "N-UNL: node " << it->first
                         << " score " << it->second;

        if (canAdd &&
            it->second < nUnlLowWaterMark &&
            nextNUnl.find(it->first) == nextNUnl.end() &&
            newValidators_.find(it->first) == newValidators_.end())
        {
            JLOG(j_.trace()) << "N-UNL: toDisable candidate " << it->first;
            toDisableCandidates.push_back(it->first);
        }

        if (it->second > nUnlHighWaterMark &&
            nextNUnl.find(it->first) != nextNUnl.end())
        {
            JLOG(j_.trace()) << "N-UNL: toReEnable candidate " << it->first;
            toReEnableCandidates.push_back(it->first);
        }
    }

    if (toReEnableCandidates.empty())
    {
        for (auto &n : nextNUnl)
        {
            if (unl.find(n) == unl.end())
            {
                toReEnableCandidates.push_back(n);
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
            JLOG(j_.trace()) << "N-UNL: add new validator " << n
                             << " at ledger seq=" << seq;
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
