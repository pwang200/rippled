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

#ifndef RIPPLE_APP_LEDGER_LEDGERREPLAYTASK_H_INCLUDED
#define RIPPLE_APP_LEDGER_LEDGERREPLAYTASK_H_INCLUDED

#include <ripple/app/ledger/InboundLedger.h>
#include <ripple/app/ledger/impl/TimeoutCounter.h>
#include <ripple/app/main/Application.h>

#include <memory>
#include <vector>

namespace ripple {
class InboundLedgers;
class Ledger;
class LedgerDeltaAcquire;
class LedgerReplayer;
class SkipListAcquire;
namespace test {
class LedgerReplayClient;
}  // namespace test

class LedgerReplayTask final
    : public TimeoutCounter,
      public std::enable_shared_from_this<LedgerReplayTask>,
      public CountedObject<LedgerReplayTask>
{
public:
    struct TaskParameter
    {
        // set on construct
        InboundLedger::Reason reason;
        uint256 finishHash;
        std::uint32_t totalLedgers;  // including the start and the finish

        // to be updated
        std::uint32_t finishSeq = 0;
        std::vector<uint256> skipList = {};  // including the finishHash
        uint256 startHash = {};
        std::uint32_t startSeq = 0;
        bool full = false;

        TaskParameter(
            InboundLedger::Reason r,
            uint256 const& finishLedgerHash,
            std::uint32_t totalNumLedgers);

        /**
         * @note called with verified skip list data
         * @param hash  hash of the ledger that has the skip list
         * @param seq  sequence number of the ledger that has the skip list
         * @param sList  skip list
         * @return false if error (e.g. hash mismatch)
         *         true on success
         */
        bool
        update(
            uint256 const& hash,
            std::uint32_t seq,
            std::vector<uint256> const& sList);

        /** check if this task can be merged into an existing task */
        bool
        canMergeInto(TaskParameter const& existingTask) const;
    };

    /**
     * Constructor
     * @param app  Application reference
     * @param inboundLedgers  InboundLedgers reference
     * @param replayer  LedgerReplayer reference
     * @param skipListAcquirer  shared_ptr of SkipListAcquire subtask,
     *        to make sure it will not be destroyed.
     * @param parameter  parameter of the task
     */
    LedgerReplayTask(
        Application& app,
        InboundLedgers& inboundLedgers,
        LedgerReplayer& replayer,
        std::shared_ptr<SkipListAcquire>& skipListAcquirer,
        TaskParameter&& parameter);

    ~LedgerReplayTask();

    /** Start the task */
    void
    init();

    /**
     * Update this task (by a SkipListAcquire subtask) when skip list is ready
     * @param hash  hash of the ledger that has the skip list
     * @param seq  sequence number of the ledger that has the skip list
     * @param sList  skip list
     */
    void
    updateSkipList(
        uint256 const& hash,
        std::uint32_t seq,
        std::vector<ripple::uint256> const& sList);

    /**
     * add a new LedgerDeltaAcquire subtask
     * @param delta  the new LedgerDeltaAcquire subtask
     * @note the LedgerDeltaAcquire subtasks must be added in order
     */
    void
    addDelta(std::shared_ptr<LedgerDeltaAcquire> const& delta);

    TaskParameter const&
    getTaskParameter() const
    {
        return parameter_;
    }

    /**
     * Notify this task (by a LedgerDeltaAcquire subtask) that a delta is ready
     * @param deltaHash  ledger hash of the delta
     */
    void
    deltaReady(uint256 const& deltaHash);

    /** Cancel the task */
    void
    cancel();

    /** return if the task is finished */
    bool
    finished() const;

    static char const*
    getCountedObjectName()
    {
        return "LedgerReplayTask";
    }

private:
    void
    onTimer(bool progress, ScopedLockType& sl) override;

    std::weak_ptr<TimeoutCounter>
    pmDowncast() override;

    /**
     * Trigger another round
     * @param sl  lock. this function must be called with the lock
     */
    void
    trigger(ScopedLockType& sl);

    /**
     * Try to build more ledgers
     * @param sl  lock. this function must be called with the lock
     */
    void
    tryAdvance(ScopedLockType& sl);

    InboundLedgers& inboundLedgers_;
    LedgerReplayer& replayer_;
    TaskParameter parameter_;
    uint32_t maxTimeouts_;
    std::shared_ptr<SkipListAcquire> skipListAcquirer_;
    std::shared_ptr<Ledger const> parent_ = {};
    uint32_t deltaToBuild = 0;  // should not build until have parent
    std::vector<std::shared_ptr<LedgerDeltaAcquire>> deltas_;

    friend class test::LedgerReplayClient;
};

}  // namespace ripple

#endif
