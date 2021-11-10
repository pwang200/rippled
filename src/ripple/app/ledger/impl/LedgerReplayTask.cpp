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

#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/LedgerReplayTask.h>
#include <ripple/app/ledger/LedgerReplayer.h>
#include <ripple/app/ledger/impl/LedgerDeltaAcquire.h>
#include <ripple/app/ledger/impl/SkipListAcquire.h>
#include <ripple/core/JobQueue.h>

namespace ripple {

LedgerReplayTask::TaskParameter::TaskParameter(
    InboundLedger::Reason r,
    uint256 const& finishLedgerHash,
    std::uint32_t totalNumLedgers)
    : reason_(r)
    , type_(hasCount)
    , startHash_()
    , finishHash_(finishLedgerHash)
    , totalLedgers_(totalNumLedgers)
{
    assert(finishLedgerHash.isNonZero() && totalNumLedgers > 0);
}

LedgerReplayTask::TaskParameter::TaskParameter(
    InboundLedger::Reason r,
    const uint256& startLedgerHash,
    const uint256& finishLedgerHash)
    : reason_(r)
    , type_(hasStart)
    , startHash_(startLedgerHash)
    , finishHash_(finishLedgerHash)
    , totalLedgers_(0)
{
    assert(startLedgerHash.isNonZero() && finishLedgerHash.isNonZero());
    //        startLedgerHash != finishLedgerHash);
}

bool
LedgerReplayTask::TaskParameter::update(
    uint256 const& hash,
    std::uint32_t seq,
    std::vector<uint256> const& sList)
{
    assert(totalLedgers_ != 0 || startHash_.isNonZero());

    if (finishHash_ != hash || full_ || sList.size() >= seq ||
        std::find(sList.begin(), sList.end(), finishHash_) != sList.end())
    {
        return false;
    }

    skipList_ = sList;
    skipList_.emplace_back(finishHash_);

    if (type_ == hasStart)
    {
        auto i = std::find(skipList_.begin(), skipList_.end(), startHash_);
        if (i == skipList_.end())
            return false;

        totalLedgers_ = skipList_.end() - i;
    }
    else
    {
        if (skipList_.size() < totalLedgers_)
            return false;

        startHash_ = skipList_[skipList_.size() - totalLedgers_];
        assert(startHash_.isNonZero());
    }

    finishSeq_ = seq;
    startSeq_ = finishSeq_ - totalLedgers_ + 1;
    full_ = true;
    return true;
}

bool
LedgerReplayTask::TaskParameter::canMergeInto(
    TaskParameter const& existingTask) const
{
    if (reason_ == existingTask.reason_)
    {
        if (type_ == hasStart)
        {
            if (startHash_ == existingTask.startHash_ &&
                finishHash_ == existingTask.finishHash_)
            {
                return true;
            }

            if (existingTask.full_)
            {
                auto const& exList = existingTask.skipList_;
                auto const startIter =
                    std::find(exList.begin(), exList.end(), startHash_);
                auto const finishIter =
                    std::find(exList.begin(), exList.end(), finishHash_);
                auto const otherStartIter = std::find(
                    exList.begin(), exList.end(), existingTask.startHash_);
                auto const otherFinishIter = std::find(
                    exList.begin(), exList.end(), existingTask.finishHash_);

                return otherStartIter <= startIter && startIter <= finishIter &&
                    finishIter <= otherFinishIter;
            }
        }
        else
        {
            if (finishHash_ == existingTask.finishHash_ &&
                totalLedgers_ <= existingTask.totalLedgers_)
            {
                return true;
            }

            if (existingTask.full_)
            {
                auto const& exList = existingTask.skipList_;
                if (auto i =
                        std::find(exList.begin(), exList.end(), finishHash_);
                    i != exList.end())
                {
                    return existingTask.totalLedgers_ >=
                        totalLedgers_ + (exList.end() - i) - 1;
                }
            }
        }
    }

    return false;
}

LedgerReplayTask::LedgerReplayTask(
    Application& app,
    InboundLedgers& inboundLedgers,
    LedgerReplayer& replayer,
    std::shared_ptr<SkipListAcquire>& skipListAcquirer,
    TaskParameter&& parameter)
    : TimeoutCounter(
          app,
          parameter.finishHash_,
          LedgerReplayParameters::TASK_TIMEOUT,
          {jtREPLAY_TASK,
           "LedgerReplayTask",
           LedgerReplayParameters::MAX_QUEUED_TASKS},
          app.journal("LedgerReplayTask"))
    , inboundLedgers_(inboundLedgers)
    , replayer_(replayer)
    , parameter_(parameter)
    , maxTimeouts_(std::max(
          LedgerReplayParameters::TASK_MAX_TIMEOUTS_MINIMUM,
          parameter.totalLedgers_ *
              LedgerReplayParameters::TASK_MAX_TIMEOUTS_MULTIPLIER))
    , skipListAcquirer_(skipListAcquirer)
{
    JLOG(journal_.trace()) << "Create " << hash_;
}

LedgerReplayTask::~LedgerReplayTask()
{
    JLOG(journal_.trace()) << "Destroy " << hash_;
}

void
LedgerReplayTask::init()
{
    JLOG(journal_.debug()) << "Task start " << hash_;

    std::weak_ptr<LedgerReplayTask> wptr = shared_from_this();
    skipListAcquirer_->addDataCallback([wptr](bool good, uint256 const& hash) {
        if (auto sptr = wptr.lock(); sptr)
        {
            if (!good)
            {
                sptr->cancel();
            }
            else
            {
                auto const skipListData = sptr->skipListAcquirer_->getData();
                sptr->updateSkipList(
                    hash, skipListData->ledgerSeq, skipListData->skipList);
            }
        }
    });

    ScopedLockType sl(mtx_);
    if (!isDone())
    {
        trigger(sl);
        setTimer(sl);
    }
}

void
LedgerReplayTask::trigger(ScopedLockType& sl)
{
    JLOG(journal_.trace()) << "trigger " << hash_;
    if (!parameter_.full_)
        return;

    if (!parent_)
    {
        parent_ = app_.getLedgerMaster().getLedgerByHash(parameter_.startHash_);
        if (!parent_)
        {
            parent_ = inboundLedgers_.acquire(
                parameter_.startHash_,
                parameter_.startSeq_,
                InboundLedger::Reason::GENERIC);
        }
        if (parent_)
        {
            JLOG(journal_.trace())
                << "Got start ledger " << parameter_.startHash_ << " for task "
                << hash_;
        }
    }

    tryAdvance(sl);
}

void
LedgerReplayTask::deltaReady(uint256 const& deltaHash)
{
    JLOG(journal_.trace()) << "Delta " << deltaHash << " ready for task "
                           << hash_;
    ScopedLockType sl(mtx_);
    if (!isDone())
        tryAdvance(sl);
}

void
LedgerReplayTask::tryAdvance(ScopedLockType& sl)
{
    JLOG(journal_.trace()) << "tryAdvance task " << hash_
                           << (parameter_.full_ ? ", full parameter"
                                                : ", waiting to fill parameter")
                           << ", deltaIndex=" << deltaToBuild_
                           << ", totalDeltas=" << deltas_.size() << ", parent "
                           << (parent_ ? parent_->info().hash : uint256());

    bool shouldTry = parent_ && parameter_.full_ &&
        parameter_.totalLedgers_ - 1 == deltas_.size();
    if (!shouldTry)
        return;

    try
    {
        for (; deltaToBuild_ < deltas_.size(); ++deltaToBuild_)
        {
            auto& delta = deltas_[deltaToBuild_];
            assert(parent_->seq() + 1 == delta->ledgerSeq_);
            if (auto l = delta->tryBuild(parent_); l)
            {
                JLOG(journal_.debug())
                    << "Task " << hash_ << " got ledger " << l->info().hash
                    << " deltaIndex=" << deltaToBuild_
                    << " totalDeltas=" << deltas_.size();
                parent_ = l;
            }
            else
                return;
        }

        complete_ = true;
        JLOG(journal_.info()) << "Completed " << hash_;
    }
    catch (std::runtime_error const&)
    {
        failed_ = true;
    }
}

void
LedgerReplayTask::updateSkipList(
    uint256 const& hash,
    std::uint32_t seq,
    std::vector<uint256> const& sList)
{
    bool fallback = false;
    uint256 fallbackHash;
    InboundLedger::Reason fallbackReason;

    {
        ScopedLockType sl(mtx_);
        if (isDone())
            return;
        if (!parameter_.update(hash, seq, sList))
        {
            failed_ = true;
            if (auto const l = app_.getLedgerMaster().getLedgerByHash(hash_); l)
            {
                JLOG(journal_.error())
                    << "Parameter update failed, but have finishLedger "
                    << hash_;
                return;
            }
            else
            {
                JLOG(journal_.error())
                    << "Parameter update failed, fallback to inboundLedgers to "
                       "only download the finishLedger "
                    << hash_;
                fallback = true;
                fallbackHash = parameter_.finishHash_;
                fallbackReason = parameter_.reason_;
            }
        }
    }

    if (fallback)
    {
        inboundLedgers_.acquire(fallbackHash, 0, fallbackReason);
    }
    else  // normal cases
    {
        replayer_.createDeltas(shared_from_this());
        ScopedLockType sl(mtx_);
        if (!isDone())
            trigger(sl);
    }
}

void
LedgerReplayTask::onTimer(bool progress, ScopedLockType& sl)
{
    JLOG(journal_.trace()) << "mTimeouts=" << timeouts_ << " for " << hash_;
    if (timeouts_ > maxTimeouts_)
    {
        failed_ = true;
        JLOG(journal_.debug())
            << "LedgerReplayTask Failed, too many timeouts " << hash_;
    }
    else
    {
        trigger(sl);
    }
}

std::weak_ptr<TimeoutCounter>
LedgerReplayTask::pmDowncast()
{
    return shared_from_this();
}

void
LedgerReplayTask::addDelta(std::shared_ptr<LedgerDeltaAcquire> const& delta)
{
    std::weak_ptr<LedgerReplayTask> wptr = shared_from_this();
    delta->addDataCallback(
        parameter_.reason_, [wptr](bool good, uint256 const& hash) {
            if (auto sptr = wptr.lock(); sptr)
            {
                if (!good)
                    sptr->cancel();
                else
                    sptr->deltaReady(hash);
            }
        });

    ScopedLockType sl(mtx_);
    if (!isDone())
    {
        JLOG(journal_.trace())
            << "addDelta task " << hash_ << " deltaIndex=" << deltaToBuild_
            << " totalDeltas=" << deltas_.size();
        assert(
            deltas_.empty() ||
            deltas_.back()->ledgerSeq_ + 1 == delta->ledgerSeq_);
        deltas_.push_back(delta);
    }
}

bool
LedgerReplayTask::finished() const
{
    ScopedLockType sl(mtx_);
    return isDone();
}

}  // namespace ripple
