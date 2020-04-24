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

namespace ripple {

class Ledger;
template <class Adaptor>
class Validations;
class RCLValidationsAdaptor;
using RCLValidations = Validations<RCLValidationsAdaptor>;
class SHAMap;
namespace test
{
class NegativeUNLVoteInternal_test;
class NegativeUNLVoteScoreTable_test;
}

/** Manager to process NegativeUNL votes. */
class NegativeUNLVote final
{
public:

    static constexpr size_t nUnlLowWaterMark = FLAG_LEDGER * 0.5;
    static constexpr size_t nUnlHighWaterMark = FLAG_LEDGER * 0.8;
    static constexpr size_t nUnlMinLocalValsToVote = FLAG_LEDGER * 0.9;
    static constexpr size_t newValidatorMeasureSkip = FLAG_LEDGER * 2;
    static constexpr float  nUnlMaxListed = 0.25;

    NegativeUNLVote(NodeID const& myId, beast::Journal j);
    ~NegativeUNLVote() = default;

    using LedgerConstPtr = std::shared_ptr<Ledger const> const;

/** Cast our local vote on the negative UNL candidates.

    @param prevLedger
    @param initialSet
*/
    void
    doVoting (LedgerConstPtr & prevLedger,
              hash_set<PublicKey> const & unl,
              RCLValidations & validations,
              std::shared_ptr<SHAMap> const& initialSet);

    void
    newValidators (LedgerIndex seq,
            hash_set<NodeID> const& nowTrusted);

private:
    NodeID const myId_;
    beast::Journal j_;
    std::mutex mutex_;
    hash_map<NodeID, LedgerIndex> newValidators_;

    void
    addTx(LedgerIndex seq,
          PublicKey const &v,
          bool disabling,
          std::shared_ptr<SHAMap> const& initialSet);

    NodeID
    pickOneCandidate(uint256 randomPadData,
                     std::vector<NodeID> & candidates);

    bool
    buildScoreTable(LedgerConstPtr & prevLedger,
                    hash_set<NodeID> const & unl,
                    RCLValidations & validations,
                    hash_map<NodeID, unsigned int> & scoreTable);

    void
    findAllCandidates(hash_set<NodeID> const& unl,
                      hash_set<NodeID> const& nextNUnl,
                      hash_map<NodeID, unsigned int> const& scoreTable,
                      std::vector<NodeID> & toDisableCandidates,
                      std::vector<NodeID> & removeCandidates);

    void
    purgeNewValidators(LedgerIndex seq);

    friend class test::NegativeUNLVoteInternal_test;
    friend class test::NegativeUNLVoteScoreTable_test;
};

} // ripple

#endif
