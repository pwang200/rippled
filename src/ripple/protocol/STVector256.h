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

#ifndef RIPPLE_PROTOCOL_STVECTOR256_H_INCLUDED
#define RIPPLE_PROTOCOL_STVECTOR256_H_INCLUDED

#include <ripple/protocol/STBitString.h>
#include <ripple/protocol/STInteger.h>
#include <ripple/protocol/STBase.h>

namespace ripple {

template <std::size_t Bits, class Tag = void>
class STVectorHugeInt
    : public STBase
{
public:
    static_assert(Bits == 256 || Bits == 160); //TODO test Tag type when Bits==160

    using int_type = base_uint<Bits, Tag>;
    using value_type = std::vector<int_type> const&;
    using ref = typename std::vector<int_type>::reference;
    using const_ref = typename std::vector<int_type>::const_reference;
    using size_type = typename std::vector<int_type>::size_type;
    using iterator = typename std::vector<int_type>::iterator;
    using const_iterator = typename std::vector<int_type>::const_iterator;

    STVectorHugeInt () = default;

    explicit STVectorHugeInt (SField const& n)
        : STBase (n)
    { }

    explicit STVectorHugeInt (std::vector<int_type> const& vector)
        : mValue (vector)
    { }

    STVectorHugeInt (SField const& n, std::vector<int_type> const& vector)
        : STBase (n), mValue (vector)
    { }

    STVectorHugeInt (SerialIter& sit, SField const& name);

    STBase*
    copy (std::size_t n, void* buf) const override
    {
        return emplace(n, buf, *this);
    }

    STBase*
    move (std::size_t n, void* buf) override
    {
        return emplace(n, buf, std::move(*this));
    }

    SerializedTypeID
    getSType () const override
    {
        if(Bits == 256)
            return STI_VECTOR256;
        else
            return STI_VECTORNodeIDs;
    }

    void
    add (Serializer& s) const override;

    Json::Value
    getJson (JsonOptions) const override;

    bool
    isEquivalent (const STBase& t) const override;

    bool
    isDefault () const override
    {
        return mValue.empty ();
    }

    STVectorHugeInt&
    operator= (std::vector<int_type> const& v)
    {
        mValue = v;
        return *this;
    }

    STVectorHugeInt&
    operator= (std::vector<int_type>&& v)
    {
        mValue = std::move(v);
        return *this;
    }

    void
    setValue (const STVectorHugeInt& v)
    {
        mValue = v.mValue;
    }

    /** Retrieve a copy of the vector we contain */
    explicit
    operator std::vector<int_type> () const
    {
        return mValue;
    }

    std::size_t
    size () const
    {
        return mValue.size ();
    }

    void
    resize (std::size_t n)
    {
        return mValue.resize (n);
    }

    bool
    empty () const
    {
        return mValue.empty ();
    }

    ref
    operator[] (size_type n)
    {
        return mValue[n];
    }

    const_ref
    operator[] (size_type n) const
    {
        return mValue[n];
    }

    std::vector<int_type> const&
    value() const
    {
        return mValue;
    }

    iterator
    insert(const_iterator pos, int_type const& value)
    {
        return mValue.insert(pos, value);
    }

    iterator
    insert(const_iterator pos, int_type&& value)
    {
        return mValue.insert(pos, std::move(value));
    }

    void
    push_back (int_type const& v)
    {
        mValue.push_back (v);
    }

    iterator
    begin()
    {
        return mValue.begin ();
    }

    const_iterator
    begin() const
    {
        return mValue.begin ();
    }

    iterator
    end()
    {
        return mValue.end ();
    }

    const_iterator
    end() const
    {
        return mValue.end ();
    }

    iterator
    erase (iterator position)
    {
        return mValue.erase (position);
    }

    void
    clear () noexcept
    {
        return mValue.clear ();
    }

private:
    std::vector<int_type> mValue;
};

//using STVector256 = STVectorHugeInt<256>;

} // ripple

#endif
