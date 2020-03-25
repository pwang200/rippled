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

#include <ripple/basics/Log.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/protocol/jss.h>
#include <ripple/protocol/STVector256.h>

namespace ripple {

template <std::size_t Bits>
STVectorHugeInt<Bits>::STVectorHugeInt(SerialIter& sit, SField const& name)
    : STBase(name)
{
    Blob data = sit.getVL ();
    auto const count = data.size () / (Bits / 8);
    mValue.reserve (count);
    Blob::iterator begin = data.begin ();
    unsigned int uStart  = 0;
    for (unsigned int i = 0; i != count; i++)
    {
        unsigned int uEnd = uStart + (Bits / 8);
        // This next line could be optimized to construct a default
        // uint256 in the vector and then copy into it
        mValue.push_back (int_type (Blob (begin + uStart, begin + uEnd)));
        uStart  = uEnd;
    }
}
template <std::size_t Bits>
void
STVectorHugeInt<Bits>::add (Serializer& s) const
{
    assert (fName->isBinary ());
    assert (fName->fieldType == STI_VECTOR256 || fName->fieldType == STI_VECTOR160);
    s.addVL (mValue.begin(), mValue.end(), mValue.size () * (Bits / 8));
}

template <std::size_t Bits>
bool
STVectorHugeInt<Bits>::isEquivalent (const STBase& t) const
{
    const STVectorHugeInt<Bits>* v = dynamic_cast<const STVectorHugeInt<Bits>*> (&t);
    return v && (mValue == v->value());
}

template <std::size_t Bits>
Json::Value
STVectorHugeInt<Bits>::getJson (JsonOptions) const
{
    Json::Value ret (Json::arrayValue);

    for (auto const& vEntry : mValue)
        ret.append (to_string (vEntry));

    return ret;
}

} // ripple
