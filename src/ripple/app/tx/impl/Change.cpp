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

#include <ripple/app/tx/impl/Change.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

NotTEC
Change::preflight (PreflightContext const& ctx)
{
    auto const ret = preflight0(ctx);
    if (!isTesSuccess(ret))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount (sfFee);
    if (!fee.native () || fee != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Change: invalid fee";
        return temBAD_FEE;
    }

    if (!ctx.tx.getSigningPubKey ().empty () ||
        !ctx.tx.getSignature ().empty () ||
        ctx.tx.isFieldPresent (sfSigners))
    {
        JLOG(ctx.j.warn()) << "Change: Bad signature";
        return temBAD_SIGNATURE;
    }

    if (ctx.tx.getSequence () != 0 || ctx.tx.isFieldPresent (sfPreviousTxnID))
    {
        JLOG(ctx.j.warn()) << "Change: Bad sequence";
        return temBAD_SEQUENCE;
    }

    return tesSUCCESS;
}

TER
Change::preclaim(PreclaimContext const &ctx)
{
    // If tapOPEN_LEDGER is resurrected into ApplyFlags,
    // this block can be moved to preflight.
    if (ctx.view.open())
    {
        JLOG(ctx.j.warn()) << "Change transaction against open ledger";
        return temINVALID;
    }

    if (ctx.tx.getTxnType() != ttAMENDMENT
        && ctx.tx.getTxnType() != ttFEE
        && ctx.tx.getTxnType() != ttNEGATIVE_UNL)
        return temUNKNOWN;

    return tesSUCCESS;
}


TER
Change::doApply()
{
    assert( ctx_.tx.getTxnType() == ttAMENDMENT ||
            ctx_.tx.getTxnType() == ttFEE ||
            ctx_.tx.getTxnType() == ttNEGATIVE_UNL);

    if (ctx_.tx.getTxnType () == ttAMENDMENT)
        return applyAmendment ();
    else if (ctx_.tx.getTxnType() == ttFEE )
        return applyFee ();
    else
    {
        assert(ctx_.tx.getTxnType() == ttNEGATIVE_UNL);
        return applyNegativeUNL();
    }
}

void
Change::preCompute()
{
    account_ = ctx_.tx.getAccountID(sfAccount);
    assert(account_ == beast::zero);
}

TER
Change::applyAmendment()
{
    uint256 amendment (ctx_.tx.getFieldH256 (sfAmendment));

    auto const k = keylet::amendments();

    SLE::pointer amendmentObject =
        view().peek (k);

    if (!amendmentObject)
    {
        amendmentObject = std::make_shared<SLE>(k);
        view().insert(amendmentObject);
    }

    STVector256 amendments =
        amendmentObject->getFieldV256(sfAmendments);

    if (std::find (amendments.begin(), amendments.end(),
            amendment) != amendments.end ())
        return tefALREADY;

    auto flags = ctx_.tx.getFlags ();

    const bool gotMajority = (flags & tfGotMajority) != 0;
    const bool lostMajority = (flags & tfLostMajority) != 0;

    if (gotMajority && lostMajority)
        return temINVALID_FLAG;

    STArray newMajorities (sfMajorities);

    bool found = false;
    if (amendmentObject->isFieldPresent (sfMajorities))
    {
        const STArray &oldMajorities = amendmentObject->getFieldArray (sfMajorities);
        for (auto const& majority : oldMajorities)
        {
            if (majority.getFieldH256 (sfAmendment) == amendment)
            {
                if (gotMajority)
                    return tefALREADY;
                found = true;
            }
            else
            {
                // pass through
                newMajorities.push_back (majority);
            }
        }
    }

    if (! found && lostMajority)
        return tefALREADY;

    if (gotMajority)
    {
        // This amendment now has a majority
        newMajorities.push_back (STObject (sfMajority));
        auto& entry = newMajorities.back ();
        entry.emplace_back (STHash256 (sfAmendment, amendment));
        entry.emplace_back (STUInt32 (sfCloseTime,
            view().parentCloseTime().time_since_epoch().count()));

        if (!ctx_.app.getAmendmentTable ().isSupported (amendment))
        {
            JLOG (j_.warn()) <<
                "Unsupported amendment " << amendment <<
                " received a majority.";
        }
    }
    else if (!lostMajority)
    {
        // No flags, enable amendment
        amendments.push_back (amendment);
        amendmentObject->setFieldV256 (sfAmendments, amendments);

        ctx_.app.getAmendmentTable ().enable (amendment);

        if (!ctx_.app.getAmendmentTable ().isSupported (amendment))
        {
            JLOG (j_.error()) <<
                "Unsupported amendment " << amendment <<
                " activated: server blocked.";
            ctx_.app.getOPs ().setAmendmentBlocked ();
        }
    }

    if (newMajorities.empty ())
        amendmentObject->makeFieldAbsent (sfMajorities);
    else
        amendmentObject->setFieldArray (sfMajorities, newMajorities);

    view().update (amendmentObject);

    return tesSUCCESS;
}

TER
Change::applyFee()
{
    auto const k = keylet::fees();

    SLE::pointer feeObject = view().peek (k);

    if (!feeObject)
    {
        feeObject = std::make_shared<SLE>(k);
        view().insert(feeObject);
    }

    feeObject->setFieldU64 (
        sfBaseFee, ctx_.tx.getFieldU64 (sfBaseFee));
    feeObject->setFieldU32 (
        sfReferenceFeeUnits, ctx_.tx.getFieldU32 (sfReferenceFeeUnits));
    feeObject->setFieldU32 (
        sfReserveBase, ctx_.tx.getFieldU32 (sfReserveBase));
    feeObject->setFieldU32 (
        sfReserveIncrement, ctx_.tx.getFieldU32 (sfReserveIncrement));

    view().update (feeObject);

    JLOG(j_.warn()) << "Fees have been changed";
    return tesSUCCESS;
}

TER
Change::applyNegativeUNL()
{
    if(view().seq() % FLAG_LEDGER != 0)
    {
        JLOG(j_.warn()) << "N-UNL: applyNegativeUNL, not a flag ledger seq="
                        << view().seq();
        return tefFAILURE;
    }

    auto const k = keylet::negativeUNL();
    SLE::pointer nUnlObject = view().peek(k);
    if (!nUnlObject)
    {
        JLOG(j_.trace()) << "N-UNL: applyNegativeUNL new nUnlObject";//TODO remove
        nUnlObject = std::make_shared<SLE>(k);
        view().insert(nUnlObject);
    }

    auto txNodeId = ctx_.tx.getFieldH160(sfNegativeUNLTxNodeID);
    bool found = false;
    if(nUnlObject->isFieldPresent(sfNegativeUNL))
    {
        auto const & nUnl = nUnlObject->getFieldV160(sfNegativeUNL);
        found = std::find(nUnl.begin(), nUnl.end(), txNodeId) != nUnl.end();
    }

    if (ctx_.tx.getFieldU8(sfNegativeUNLTxAdd))
    {
        // one add Tx
        if(nUnlObject->isFieldPresent(sfNegativeUNLToAdd))
        {
            JLOG(j_.warn()) << "N-UNL: applyNegativeUNL, already has ToAdd "
                        << nUnlObject->getFieldH160(sfNegativeUNLToAdd)
                        << " trying to add "
                        << txNodeId;
            return tefFAILURE;
        }

        // cannot be the same as toRemove
        if(nUnlObject->isFieldPresent(sfNegativeUNLToRemove))
        {
            if(nUnlObject->getFieldH160(sfNegativeUNLToRemove) == txNodeId)
            {
                JLOG(j_.warn())
                    << "N-UNL: applyNegativeUNL, ToAdd is same as ToRemove "
                    << txNodeId;
                return tefFAILURE;
            }
        }

        // cannot be already in nUNL
        if(found)
        {
            JLOG(j_.warn())
                    << "N-UNL: applyNegativeUNL, ToAdd is already in nUNL "
                    << txNodeId;
                return tefFAILURE;
        }

        nUnlObject->setFieldH160(sfNegativeUNLToAdd, txNodeId);
        JLOG(j_.info()) << "N-UNL: applyNegativeUNL Tx add "
                         << txNodeId;
    }
    else
    {
        if(nUnlObject->isFieldPresent(sfNegativeUNLToRemove))
        {
            JLOG(j_.warn()) << "N-UNL: applyNegativeUNL, already has ToRemove "
                            << nUnlObject->getFieldH160(sfNegativeUNLToRemove)
                            << " trying to remove "
                            << txNodeId;
            return tefFAILURE;
        }

        // cannot be the same as toAdd
        if(nUnlObject->isFieldPresent(sfNegativeUNLToAdd))
        {
            if(nUnlObject->getFieldH160(sfNegativeUNLToAdd) == txNodeId)
            {
                JLOG(j_.warn())
                    << "N-UNL: applyNegativeUNL, ToRemove is same as ToAdd "
                    << txNodeId;
                return tefFAILURE;
            }
        }

        // must be already in nUNL
        if(! found)
        {
            JLOG(j_.warn())
                    << "N-UNL: applyNegativeUNL, ToRemove is not in nUNL "
                    << txNodeId;
                return tefFAILURE;
        }

        nUnlObject->setFieldH160(sfNegativeUNLToRemove,
                ctx_.tx.getFieldH160(sfNegativeUNLTxNodeID));
        JLOG(j_.info()) << "N-UNL: applyNegativeUNL Tx remove "
                         << txNodeId;
    }
    view().update(nUnlObject);

    JLOG(j_.trace()) << "N-UNL: applyNegativeUNL Tx done.";//TODO remove
    return tesSUCCESS;
}

}
