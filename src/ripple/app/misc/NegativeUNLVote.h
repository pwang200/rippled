//------------------------------------------------------------------------------
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

#ifndef RIPPLE_APP_MISC_NEGATIVEUNLVOTE_H_INCLUDED
#define RIPPLE_APP_MISC_NEGATIVEUNLVOTE_H_INCLUDED

#include <ripple/beast/utility/Journal.h>
//namespace beast {
//    class Journal;
//}

namespace ripple {

class Ledger;
template <class Adaptor>
class Validations;
class RCLValidationsAdaptor;
using RCLValidations = Validations<RCLValidationsAdaptor>;
class SHAMap;
namespace test
{
class NegativeUNLVote_test;
}

/** Manager to process NegativeUNL votes. */
class NegativeUNLVote final
{
public:
//TODO consider put in a struct
    static constexpr size_t nUnlLowWaterMark = FLAG_LEDGER * 0.5;
    static constexpr size_t nUnlHighWaterMark = FLAG_LEDGER * 0.8;
    static constexpr size_t nUnlMinLocalValsToVote = FLAG_LEDGER * 0.95;
    static constexpr size_t newValidatorMeasureSkip = FLAG_LEDGER * 2;
    static constexpr float  nUnlMaxListed = 0.25;

    ~NegativeUNLVote() = default;
    NegativeUNLVote(NodeID const& myId,
            RCLValidations & validations,
            beast::Journal j);

    NegativeUNLVote() = delete;
    NegativeUNLVote(NegativeUNLVote const&) = delete;
    NegativeUNLVote&
    operator=(NegativeUNLVote const&) = delete;

    using LedgerConstPtr = std::shared_ptr<Ledger const> const;
/** Cast our local vote on the negative UNL candidates.

    @param prevLedger
    @param initialSet
*/
    void
    doVoting (LedgerConstPtr & prevLedger,
              hash_set<NodeID> const & unl,
              std::shared_ptr<SHAMap> const& initialSet);

    void
    newValidators (LedgerIndex seq,
            hash_set<NodeID> const& nowTrusted);

private:
    NodeID const myId_;
    RCLValidations & validations_;
    beast::Journal j_;
    std::mutex mutex_;
    hash_map<NodeID, LedgerIndex> newValidators_;

    void
    addTx(LedgerIndex seq,
          NodeID const &nid,
          bool adding,
          std::shared_ptr<SHAMap> const& initialSet);
    NodeID
    pickOneCandidate(uint256 randomPadData,
            std::vector<NodeID> & candidates);
    bool
    buildScoreTable(LedgerConstPtr & prevLedger,
            hash_set<NodeID> const & unl,
            hash_map<NodeID, unsigned int> & scoreTable);

    void
    findAllCandidates(//LedgerIndex seq,
            //LedgerConstPtr & prevLedger,
            hash_set<NodeID> const& unl,
            hash_set<NodeID> const& nextNUnl,
            hash_map<NodeID, unsigned int> const& scoreTable,
            std::vector<NodeID> & addCandidates,
            std::vector<NodeID> & removeCandidates);

    void
    purgeNewValidators(LedgerIndex seq);

    friend class test::NegativeUNLVote_test;
};

} // ripple

#endif
