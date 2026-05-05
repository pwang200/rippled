# `get*Field` host function wrappers: avoid the intermediate `Bytes`

A proposed code optimization in `src/libxrpl/tx/wasm/`. Independent of
any gas-pricing or fee work — purely an efficiency / fast-fail
improvement for the WASM host-function wrappers.

## Problem

Every `Bytes`-returning host function today goes through a two-step
output path with one heap allocation and two memcpys per call:

```
SLE field bytes  --[getAnyFieldData: alloc Bytes + memcpy]-->  Bytes
Bytes            --[setData: memcpy]-->                         wasm linear memory
```

The intermediate `Bytes` is a C++ convenience — the wasm caller has
already supplied the destination buffer (`out_ptr`, `out_len`) in its
own linear memory, so the impl could write directly into that buffer
with a single copy.

Concretely, the impl interface in `include/xrpl/tx/wasm/HostFuncImpl.h`
declares:

```cpp
Expected<Bytes, HostFunctionError>
getCurrentLedgerObjField(SField const& fname) const override;
```

and `src/libxrpl/tx/wasm/HostFuncImplGetter.cpp` implements it as:

```cpp
WasmHostFunctionsImpl::getCurrentLedgerObjField(SField const& fname) const
{
    auto const sle = getCurrentLedgerObj();
    if (!sle.has_value())
        return Unexpected(sle.error());
    return getAnyFieldData(sle.value()->peekAtPField(fname));
    //     ^-- allocates a Bytes + memcpys field bytes into it
}
```

The corresponding wrapper in `src/libxrpl/tx/wasm/HostFuncWrapper.cpp`
(search for `getCurrentLedgerObjField_wrap`) then calls
`returnResult(...)`, which falls through to `setData()` (near the top
of the file) — the second memcpy, host → wasm linear memory.

## Affected wrappers

All `Bytes`-returning host fns whose result eventually goes through
`returnResult`'s `Bytes` branch in
`src/libxrpl/tx/wasm/HostFuncWrapper.cpp`. Search the impl header for
`Expected<Bytes, HostFunctionError>` to enumerate. As of this writing:

**Variable-size returns (where the refactor matters most):**
- `getTxField`, `getCurrentLedgerObjField`, `getLedgerObjField`
- `getTxNestedField`, `getCurrentLedgerObjNestedField`, `getLedgerObjNestedField`
- `getNFT`, `getNFTIssuer`

**Fixed-size returns (refactor still helpful but lower payoff):**
- `accountKeylet`, `ammKeylet`, `checkKeylet`, `credentialKeylet`,
  `delegateKeylet`, `depositPreauthKeylet`, `didKeylet`, `escrowKeylet`,
  `lineKeylet`, `mptIssuanceKeylet`, `mptokenKeylet`, `nftOfferKeylet`,
  `offerKeylet`, `oracleKeylet`, `paychanKeylet`,
  `permissionedDomainKeylet`, `signersKeylet`, `ticketKeylet`,
  `vaultKeylet` — all return a 32-byte keylet hash.

**Float family (variable but bounded by float serialization size):**
- `floatFromInt`, `floatFromUint`, `floatFromSTAmount`,
  `floatFromSTNumber`, `floatNegate`, `floatAbs`, `floatSet`,
  `floatAdd`, `floatSubtract`, `floatMultiply`, `floatDivide`,
  `floatRoot`, `floatPower`, `floatLog`

## Proposed signature

Change the impl signature from returning an owned `Bytes` to writing
directly into a caller-provided buffer:

```cpp
// include/xrpl/tx/wasm/HostFuncImpl.h:
Expected<int32_t, HostFunctionError>
getCurrentLedgerObjField(SField const& fname, uint8_t* dst, std::size_t cap)
    const override;
```

Returns the number of bytes actually written, or
`HostFunctionError::DATA_FIELD_TOO_LARGE` if `cap` is insufficient.
The impl serializes the field directly into `dst[0 .. written]`.

### Wrapper changes

In `src/libxrpl/tx/wasm/HostFuncWrapper.cpp`, each affected `*_wrap`
function already reads `out_ptr` and `out_len` from `params`. The
change is:

- Map `out_ptr` to a host pointer via `runtime->getMem().p + out_ptr`.
- Pass that pointer plus `out_len` as `cap` to the impl.
- `returnResult`'s `Bytes` branch can be removed once no impl returns
  `Bytes`. During the transition both shapes can coexist.

## Side benefit: fast-fail on oversize

Today, when a caller requests a field whose serialized size exceeds
the wasm-side cap, the impl still does the full per-byte work before
`setData` rejects:

```cpp
auto const result = hf->getCurrentLedgerObjField(*fname);
//                  ^-- allocates Bytes + memcpys field into it
//                      (byte-proportional cost happens HERE, before
//                      any size check against out_len)
//
return returnResult(runtime, params, results, result, index);
//                  ^-- setData rejects if result.size() > maxWasmDataLength
//                      OR > out_len, but the host has already done the work.
```

The grief-protection benefit of the size cap is lost: a caller pays
the entry gas for an oversized request, but the network does the
byte-proportional work anyway.

After the refactor, the impl receives `cap` up-front and can
short-circuit:

```cpp
auto const& field = sle->peekAtPField(fname);
auto const sz = field.getSerializedSize();  // or whatever cheap accessor
if (sz > cap)
    return Unexpected(HostFunctionError::DATA_FIELD_TOO_LARGE);
// ... only now serialize into dst
```

(If `STBlob` / `STAccount` / etc. don't already expose a cheap-size
accessor that doesn't require materializing the bytes, that's a small
prerequisite — worth ~30 minutes investigation of
`src/libxrpl/protocol/STBase.h`, `STBlob.h`, `STAccount.h`.)

## Measured impact

A microbenchmark of `getCurrentLedgerObjField_wrap` against an
in-memory `OpenView` populated with synthetic SLEs (Apple M4 Pro,
release build, tight inner loop with statistical sampling) showed:

```
field size (bytes) |  median ns | IQR/median%
-------------------+------------+-------------
                32 |         35 | 20.00%
               128 |         28 |  7.14%
               512 |         39 |  2.56%
              1024 |         51 |  0.00%
              4096 |        103 | 25.24%
              8192 |        109 | 24.77%   [over limit; setData rejects after host copy]
```

Two things to note:

1. The current per-call cost has a per-byte component on the
   wrapper-internal allocate-and-copy path. Removing the intermediate
   `Bytes` collapses two memcpys into one and removes one small heap
   allocation, which should approximately halve the per-byte slope
   and reduce the fixed intercept.
2. The 8 KB over-limit row (109 ns) is *slower* than the largest
   in-bounds row (4 KB at 103 ns), not faster. A correctly fast-failing
   rejection would land near the fixed intercept (~30 ns). The refactor
   delivers that automatically.

## Verification

1. Existing impl tests in `src/test/app/HostFuncImpl_test.cpp`
   should keep passing — they exercise these wrappers with the `ww()`
   helper. Update expectations as needed; the wrappers still write the
   same bytes into wasm memory, only the *path* changed.
2. Full `rippled --unittest` for any escrow / wasm integration tests.
3. (Optional) Re-run microbenchmarks before and after to confirm slope
   and intercept drop, and that the over-limit row drops to roughly
   the fixed intercept.

## Pitfalls

- The impl may compose multiple operations producing a temporary
  `Bytes` internally (e.g., float arithmetic). The new signature still
  works, but the impl's internal data flow may need a rework — write
  to a temporary stack buffer first, then memcpy out into `dst` once
  the size is known.
- Watch for impls that use the returned `Bytes`' size to indicate
  semantic content (e.g., empty `Bytes` meaning "field not present").
  The new signature returns `int32_t` byte count; confirm 0-byte
  returns still encode the same semantic.
- `mptokenKeylet_wrap` already validates input slice is `MPTID::bytes`
  fixed-size before calling impl — not a blocker, just keep in mind.
- Estimated scope: ~1 day if mechanical across all wrappers; longer if
  the impl/test pairs have hidden assumptions about returning `Bytes`
  by value.