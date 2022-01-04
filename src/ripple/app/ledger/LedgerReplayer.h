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

#ifndef RIPPLE_APP_LEDGER_LEDGERREPLAYER_H_INCLUDED
#define RIPPLE_APP_LEDGER_LEDGERREPLAYER_H_INCLUDED

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LedgerReplayTask.h>
#include <ripple/app/main/Application.h>
#include <ripple/beast/utility/Journal.h>

#include <memory>
#include <mutex>
#include <vector>

namespace ripple {

namespace test {
class LedgerReplayClient;
}  // namespace test

namespace LedgerReplayParameters {
// timeout value for LedgerReplayTask
auto constexpr TASK_TIMEOUT = std::chrono::milliseconds{500};

// for LedgerReplayTask to calculate max allowed timeouts
// = max( TASK_MAX_TIMEOUTS_MINIMUM,
//        (# of ledger to replay) * TASK_MAX_TIMEOUTS_MULTIPLIER)
std::uint32_t constexpr TASK_MAX_TIMEOUTS_MULTIPLIER = 2;
std::uint32_t constexpr TASK_MAX_TIMEOUTS_MINIMUM = 10;

// timeout value for subtasks: LedgerDeltaAcquire and SkipListAcquire
auto constexpr SUB_TASK_TIMEOUT = std::chrono::milliseconds{250};
// max of allowed subtask timeouts
std::uint32_t constexpr SUB_TASK_MAX_TIMEOUTS = 10;

// max number of peers that do not support the ledger replay feature
// returned by the PeerSet before switch to fallback
auto constexpr MAX_NO_FEATURE_PEER_COUNT = 2;
// subtask timeout value after fallback
auto constexpr SUB_TASK_FALLBACK_TIMEOUT = std::chrono::milliseconds{1000};

// for LedgerReplayer to limit the number of LedgerReplayTask and sub-tasks
std::uint32_t constexpr MAX_TASKS = 1000;

// for LedgerReplayer to limit the number of skiplists to retrieve in one task
std::uint32_t constexpr MAX_TASK_SKIPLISTS = 8;

// for LedgerReplayer to limit the number of ledgers to replay in one task
std::uint32_t constexpr MAX_TASK_SIZE = 256 * MAX_TASK_SKIPLISTS;

// to limit the number of LedgerReplay related jobs in JobQueue
std::uint32_t constexpr MAX_QUEUED_TASKS = 100;
}  // namespace LedgerReplayParameters

/**
 * Manages the lifetime of ledger replay tasks.
 */
class LedgerReplayer final
{
public:
    LedgerReplayer(
        Application& app,
        InboundLedgers& inboundLedgers,
        std::unique_ptr<PeerSetBuilder> peerSetBuilder);

    ~LedgerReplayer();

    /**
     * Replay a range of ledgers
     *
     * @param r  reason for the replay request
     * @param finishLedgerHash  hash of the last ledger
     * @param totalNumLedgers  total number of ledgers in the range, inclusive
     *
     * @note totalNumLedgers must > 0 && totalNumLedgers must <= 256
     *
     * @note try to replay, but in case of failures or lack of peers
     *       with forward ledger replay enabled and have the ledgers,
     *       fallback to inboundLedgers to download the finishLedger.
     *       Depending on timing, some of the ledgers between have
     *       and want may also be downloaded.
     *
     * @note when fallback to inboundLedgers to download the finishLedger,
     *       we waste one round trip for acquiring a skipList
     */
    void
    replay(
        InboundLedger::Reason r,
        uint256 const& finishLedgerHash,
        std::uint32_t totalNumLedgers);

    /**
     * Replay a range of ledgers
     *
     * @param r  reason for the replay request
     * @param startLedgerHash  hash of the first ledger
     * @param finishLedgerHash  hash of the last ledger
     *
     * @note try to replay, but in case of failures or lack of peers
     *       with forward ledger replay enabled and have the ledgers,
     *       fallback to inboundLedgers to download the finishLedger.
     *       Depending on timing, some of the ledgers between have
     *       and want may also be downloaded.
     *
     * @note when fallback to inboundLedgers to download the finishLedger,
     *       we waste one round trip for acquiring a skipList
     */
    void
    replay(
        InboundLedger::Reason r,
        uint256 const& startLedgerHash,
        uint256 const& finishLedgerHash);

    /** Create LedgerDeltaAcquire subtasks for the LedgerReplayTask task */
    void
    createDeltas(std::shared_ptr<LedgerReplayTask> task);

    /** Create a SkipListAcquire subtask for the LedgerReplayTask task */
    void
    createSkipList(
        uint256 const& target,
        std::shared_ptr<LedgerReplayTask> task);

    /**
     * Process a skip list (extracted from a TMProofPathResponse message)
     * @param info  ledger info
     * @param data  skip list holder
     * @note  info and data must have been verified against the ledger hash
     */
    void
    gotSkipList(
        LedgerInfo const& info,
        std::shared_ptr<SHAMapItem const> const& data);

    /**
     * Process a ledger delta (extracted from a TMReplayDeltaResponse message)
     * @param info  ledger info
     * @param txns  set of Txns of the ledger
     * @note info and txns must have been verified against the ledger hash
     */
    void
    gotReplayDelta(
        LedgerInfo const& info,
        std::map<std::uint32_t, std::shared_ptr<STTx const>>&& txns);

    /** Remove completed tasks */
    void
    sweep();

    void
    stop();

private:
    void
    replayInternal(LedgerReplayTask::TaskParameter&& parameter);

    mutable std::mutex mtx_;
    std::vector<std::shared_ptr<LedgerReplayTask>> tasks_;
    hash_map<uint256, std::weak_ptr<LedgerDeltaAcquire>> deltas_;
    hash_map<uint256, std::weak_ptr<SkipListAcquire>> skipLists_;

    Application& app_;
    InboundLedgers& inboundLedgers_;
    std::unique_ptr<PeerSetBuilder> peerSetBuilder_;
    beast::Journal j_;

    friend class test::LedgerReplayClient;
};

}  // namespace ripple

#endif
