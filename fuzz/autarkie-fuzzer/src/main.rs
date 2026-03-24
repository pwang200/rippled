/// Compute rippled AccountIDs from passphrase strings, matching the C++
/// `Account("alice")` derivation exactly.
///
/// Algorithm: passphrase → SHA512[0..16] (seed) → iterative SHA512-Half
/// key derivation (rippled Generator) → secp256k1 pubkey → RIPEMD160(SHA256(pubkey))
///
/// Memory layout (prepended before fuzzer-generated random data):
///   offset 0:   alice  AccountID (20 bytes)
///   offset 20:  bob    AccountID (20 bytes)
///   offset 40:  carol  AccountID (20 bytes)
///   offset 60:  gw     AccountID (20 bytes)
///   offset 80:  scratch space for keylet output (32 bytes, reused by preamble)
///   offset 112: padding (16 bytes)
///   offset 128: fuzzer-generated random memory begins
///
/// The preamble in render() uses the account IDs as input to keylet host
/// functions, then caches the resulting objects in slots 1-8.
mod known_ids {
    use ripemd::Ripemd160;
    use secp256k1::{Secp256k1, SecretKey, PublicKey};
    use sha2::{Sha256, Sha512, Digest};

    /// Replicate rippled's `generateSeed(passphrase)`: SHA512(passphrase)[0..16].
    fn generate_seed(passphrase: &str) -> [u8; 16] {
        let hash = Sha512::digest(passphrase.as_bytes());
        let mut seed = [0u8; 16];
        seed.copy_from_slice(&hash[..16]);
        seed
    }

    /// SHA512-Half: first 32 bytes of SHA512.
    fn sha512_half(data: &[u8]) -> [u8; 32] {
        let hash = Sha512::digest(data);
        let mut out = [0u8; 32];
        out.copy_from_slice(&hash[..32]);
        out
    }

    /// Append a big-endian u32 to a buffer at `offset`.
    fn put_be32(buf: &mut [u8], offset: usize, val: u32) {
        buf[offset..offset + 4].copy_from_slice(&val.to_be_bytes());
    }

    /// Replicate rippled's `deriveDeterministicRootKey`:
    /// SHA512Half(seed || counter) until valid secp256k1 key.
    fn derive_root_key(seed: &[u8; 16]) -> SecretKey {
        let secp = Secp256k1::new();
        let mut buf = [0u8; 20]; // 16-byte seed + 4-byte counter
        buf[..16].copy_from_slice(seed);
        for seq in 0u32..128 {
            put_be32(&mut buf, 16, seq);
            let hash = sha512_half(&buf);
            if let Ok(sk) = SecretKey::from_slice(&hash) {
                let _ = &secp; // validate context
                return sk;
            }
        }
        panic!("derive_root_key: no valid key found");
    }

    /// Replicate rippled's `Generator::calculateTweak`:
    /// SHA512Half(compressed_pubkey || seq || counter) until valid.
    fn calculate_tweak(generator: &[u8; 33], seq: u32) -> SecretKey {
        let mut buf = [0u8; 41]; // 33-byte pubkey + 4-byte seq + 4-byte counter
        buf[..33].copy_from_slice(generator);
        put_be32(&mut buf, 33, seq);
        for subseq in 0u32..128 {
            put_be32(&mut buf, 37, subseq);
            let hash = sha512_half(&buf);
            if let Ok(sk) = SecretKey::from_slice(&hash) {
                return sk;
            }
        }
        panic!("calculate_tweak: no valid tweak found");
    }

    /// Derive the AccountID (20 bytes) from a passphrase, matching rippled's
    /// `Account(name)` constructor exactly.
    pub fn account_id(passphrase: &str) -> [u8; 20] {
        let secp = Secp256k1::new();
        let seed = generate_seed(passphrase);

        // Step 1: root key + compressed generator public key
        let root = derive_root_key(&seed);
        let generator_pubkey = PublicKey::from_secret_key(&secp, &root);
        let generator_bytes: [u8; 33] = generator_pubkey.serialize();

        // Step 2: tweak for ordinal 0, add to root
        let tweak = calculate_tweak(&generator_bytes, 0);
        let mut private_key = root;
        private_key = private_key.add_tweak(&tweak.into()).expect("tweak add");

        // Step 3: compressed public key of derived key
        let pubkey = PublicKey::from_secret_key(&secp, &private_key);
        let pubkey_bytes = pubkey.serialize(); // 33 bytes compressed

        // Step 4: RIPEMD160(SHA256(pubkey))
        let sha = Sha256::digest(&pubkey_bytes);
        let ripe = Ripemd160::digest(&sha);
        let mut id = [0u8; 20];
        id.copy_from_slice(&ripe);
        id
    }

    /// Byte offset of the scratch area used by the preamble for keylet output.
    pub const SCRATCH: usize = 80;

    /// Total bytes of the seed region (account IDs + scratch + padding).
    pub const SEED_SIZE: usize = 128;

    /// Build the seed region as a byte vector.
    pub fn seed_bytes() -> Vec<u8> {
        let mut buf = vec![0u8; SEED_SIZE];
        buf[0..20].copy_from_slice(&account_id("alice"));
        buf[20..40].copy_from_slice(&account_id("bob"));
        buf[40..60].copy_from_slice(&account_id("carol"));
        buf[60..80].copy_from_slice(&account_id("gateway"));
        // 80..112: scratch (zeroed, written at runtime by preamble)
        // 112..128: padding
        buf
    }

    /// Host functions needed by the preamble (name, signature).
    /// Always imported even if the fuzzer didn't generate calls to them.
    pub const PREAMBLE_IMPORTS: &[(&str, &str)] = &[
        ("account_keylet",          "(func $account_keylet (param i32 i32 i32 i32) (result i32))"),
        ("did_keylet",              "(func $did_keylet (param i32 i32 i32 i32) (result i32))"),
        ("signers_keylet",          "(func $signers_keylet (param i32 i32 i32 i32) (result i32))"),
        ("deposit_preauth_keylet",  "(func $deposit_preauth_keylet (param i32 i32 i32 i32 i32 i32) (result i32))"),
        ("oracle_keylet",           "(func $oracle_keylet (param i32 i32 i32 i32 i32) (result i32))"),
        ("cache_ledger_obj",        "(func $cache_ledger_obj (param i32 i32 i32) (result i32))"),
    ];

    /// Generate WAT preamble that computes keylets and caches objects.
    ///
    /// After the preamble runs, cache slots 1-8 hold:
    ///   1: alice account    5: alice DID
    ///   2: bob account      6: alice signers
    ///   3: carol account    7: deposit_preauth(bob→alice)
    ///   4: gw account       8: oracle(alice, doc_id=1)
    pub fn preamble_wat() -> String {
        let s = SCRATCH;
        let mut w = String::new();
        w.push_str("    ;; === PREAMBLE: compute keylets and cache objects ===\n");

        let mut cache_account = |acc_offset: usize, slot: u32, label: &str| {
            w.push_str(&format!("    ;; slot {}: {} account\n", slot, label));
            w.push_str(&format!(
                "    (call $account_keylet (i32.const {}) (i32.const 20) (i32.const {}) (i32.const 32))\n    drop\n",
                acc_offset, s
            ));
            w.push_str(&format!(
                "    (call $cache_ledger_obj (i32.const {}) (i32.const 32) (i32.const {}))\n    drop\n",
                s, slot
            ));
        };

        cache_account(0,  1, "alice");
        cache_account(20, 2, "bob");
        cache_account(40, 3, "carol");
        cache_account(60, 4, "gw");

        w.push_str(&format!("    ;; slot 5: alice DID\n"));
        w.push_str(&format!(
            "    (call $did_keylet (i32.const 0) (i32.const 20) (i32.const {}) (i32.const 32))\n    drop\n", s));
        w.push_str(&format!(
            "    (call $cache_ledger_obj (i32.const {}) (i32.const 32) (i32.const 5))\n    drop\n", s));

        w.push_str(&format!("    ;; slot 6: alice signers\n"));
        w.push_str(&format!(
            "    (call $signers_keylet (i32.const 0) (i32.const 20) (i32.const {}) (i32.const 32))\n    drop\n", s));
        w.push_str(&format!(
            "    (call $cache_ledger_obj (i32.const {}) (i32.const 32) (i32.const 6))\n    drop\n", s));

        w.push_str(&format!("    ;; slot 7: deposit_preauth(bob, alice)\n"));
        w.push_str(&format!(
            "    (call $deposit_preauth_keylet (i32.const 20) (i32.const 20) (i32.const 0) (i32.const 20) (i32.const {}) (i32.const 32))\n    drop\n", s));
        w.push_str(&format!(
            "    (call $cache_ledger_obj (i32.const {}) (i32.const 32) (i32.const 7))\n    drop\n", s));

        w.push_str(&format!("    ;; slot 8: oracle(alice, doc_id=1)\n"));
        w.push_str(&format!(
            "    (call $oracle_keylet (i32.const 0) (i32.const 20) (i32.const 1) (i32.const {}) (i32.const 32))\n    drop\n", s));
        w.push_str(&format!(
            "    (call $cache_ledger_obj (i32.const {}) (i32.const 32) (i32.const 8))\n    drop\n", s));

        w.push_str("    ;; === END PREAMBLE ===\n");
        w
    }
}

/// Enumeration of all WASM host functions available in the rippled WASM runtime.
/// Each variant represents a host function with its parameters.
#[derive(Debug, Clone, autarkie::Grammar, serde::Serialize, serde::Deserialize)]
pub enum HostFunction {
    // ========================================
    // Ledger Query Functions
    // ========================================

    /// Get the current ledger sequence number
    GetLedgerSqn,

    /// Get the parent ledger time
    GetParentLedgerTime,

    /// Get the parent ledger hash
    GetParentLedgerHash {
        out_ptr: i32,
        out_size: i32,
    },

    /// Get the base fee for transactions
    GetBaseFee,

    /// Check if an amendment is enabled
    IsAmendmentEnabled {
        amendment_ptr: i32,
        amendment_size: i32,
    },

    // ========================================
    // Ledger Object Cache Functions
    // ========================================

    /// Cache a ledger object by its ID
    CacheLedgerObj {
        id_ptr: i32,
        id_size: i32,
        cache: i32,
    },

    // ========================================
    // Field Access Functions
    // ========================================

    /// Get a field from the current transaction
    GetTxField {
        field_code: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get a field from the current ledger object
    GetCurrentLedgerObjField {
        field_code: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get a field from a cached ledger object
    GetLedgerObjField {
        cache: i32,
        field_code: i32,
        out_ptr: i32,
        out_size: i32,
    },

    // ========================================
    // Nested Field Access Functions
    // ========================================

    /// Get a nested field from the current transaction
    GetTxNestedField {
        locator_ptr: i32,
        locator_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get a nested field from the current ledger object
    GetCurrentLedgerObjNestedField {
        locator_ptr: i32,
        locator_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get a nested field from a cached ledger object
    GetLedgerObjNestedField {
        cache: i32,
        locator_ptr: i32,
        locator_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    // ========================================
    // Array Length Functions
    // ========================================

    /// Get the length of an array field in the current transaction
    GetTxArrayLen {
        field_code: i32,
    },

    /// Get the length of an array field in the current ledger object
    GetCurrentLedgerObjArrayLen {
        field_code: i32,
    },

    /// Get the length of an array field in a cached ledger object
    GetLedgerObjArrayLen {
        cache: i32,
        field_code: i32,
    },

    // ========================================
    // Nested Array Length Functions
    // ========================================

    /// Get the length of a nested array in the current transaction
    GetTxNestedArrayLen {
        locator_ptr: i32,
        locator_size: i32,
    },

    /// Get the length of a nested array in the current ledger object
    GetCurrentLedgerObjNestedArrayLen {
        locator_ptr: i32,
        locator_size: i32,
    },

    /// Get the length of a nested array in a cached ledger object
    GetLedgerObjNestedArrayLen {
        cache: i32,
        locator_ptr: i32,
        locator_size: i32,
    },

    // ========================================
    // Update Function
    // ========================================

    /// Update the ledger object data
    UpdateData {
        data_ptr: i32,
        data_size: i32,
    },

    // ========================================
    // Cryptographic Functions
    // ========================================

    /// Verify a signature
    CheckSignature {
        msg_ptr: i32,
        msg_size: i32,
        sig_ptr: i32,
        sig_size: i32,
        key_ptr: i32,
        key_size: i32,
    },

    /// Compute SHA-512 half hash
    ComputeSha512HalfHash {
        data_ptr: i32,
        data_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    // ========================================
    // Keylet Functions
    // ========================================

    /// Get keylet for an account
    AccountKeylet {
        acc_ptr: i32,
        acc_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an AMM pool
    AmmKeylet {
        asset1_ptr: i32,
        asset1_size: i32,
        asset2_ptr: i32,
        asset2_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a check
    CheckKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a credential
    CredentialKeylet {
        subj_ptr: i32,
        subj_size: i32,
        iss_ptr: i32,
        iss_size: i32,
        cred_ptr: i32,
        cred_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a delegate
    DelegateKeylet {
        acc_ptr: i32,
        acc_size: i32,
        auth_ptr: i32,
        auth_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a deposit preauth
    DepositPreauthKeylet {
        acc_ptr: i32,
        acc_size: i32,
        auth_ptr: i32,
        auth_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a DID
    DidKeylet {
        acc_ptr: i32,
        acc_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an escrow
    EscrowKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a trust line
    LineKeylet {
        acc1_ptr: i32,
        acc1_size: i32,
        acc2_ptr: i32,
        acc2_size: i32,
        curr_ptr: i32,
        curr_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an MPT issuance
    MptIssuanceKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an MPToken
    MptokenKeylet {
        mptid_ptr: i32,
        mptid_size: i32,
        holder_ptr: i32,
        holder_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an NFT offer
    NftOfferKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an offer
    OfferKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for an oracle
    OracleKeylet {
        acc_ptr: i32,
        acc_size: i32,
        doc_id: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a payment channel
    PaychanKeylet {
        acc_ptr: i32,
        acc_size: i32,
        dest_ptr: i32,
        dest_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a permissioned domain
    PermissionedDomainKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for signers
    SignersKeylet {
        acc_ptr: i32,
        acc_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a ticket
    TicketKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get keylet for a vault
    VaultKeylet {
        acc_ptr: i32,
        acc_size: i32,
        seq: i32,
        out_ptr: i32,
        out_size: i32,
    },

    // ========================================
    // NFT Functions
    // ========================================

    /// Get NFT information
    GetNFT {
        acc_ptr: i32,
        acc_size: i32,
        nftid_ptr: i32,
        nftid_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get NFT issuer
    GetNFTIssuer {
        nftid_ptr: i32,
        nftid_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get NFT taxon
    GetNFTTaxon {
        nftid_ptr: i32,
        nftid_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    /// Get NFT flags
    GetNFTFlags {
        nftid_ptr: i32,
        nftid_size: i32,
    },

    /// Get NFT transfer fee
    GetNFTTransferFee {
        nftid_ptr: i32,
        nftid_size: i32,
    },

    /// Get NFT serial number
    GetNFTSerial {
        nftid_ptr: i32,
        nftid_size: i32,
        out_ptr: i32,
        out_size: i32,
    },

    // ========================================
    // Trace/Debug Functions
    // ========================================

    /// Trace/log arbitrary data
    Trace {
        msg_ptr: i32,
        msg_size: i32,
        data_ptr: i32,
        data_size: i32,
        as_hex: i32,
    },

    /// Trace/log a number
    TraceNum {
        msg_ptr: i32,
        msg_size: i32,
        number: i64,
    },

    /// Trace/log an account
    TraceAccount {
        msg_ptr: i32,
        msg_size: i32,
        acc_ptr: i32,
        acc_size: i32,
    },

    /// Trace/log a float
    TraceFloat {
        msg_ptr: i32,
        msg_size: i32,
        float_ptr: i32,
        float_size: i32,
    },

    /// Trace/log an amount
    TraceAmount {
        msg_ptr: i32,
        msg_size: i32,
        amount_ptr: i32,
        amount_size: i32,
    },

    // ========================================
    // Float Arithmetic Functions
    // ========================================

    /// Convert signed integer to float
    FloatFromInt {
        x: i64,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Convert unsigned integer to float
    FloatFromUint {
        x_ptr: i32,
        x_size: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Create float from mantissa and exponent
    FloatSet {
        exp: i32,
        mant: i64,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Compare two floats
    FloatCompare {
        x_ptr: i32,
        x_size: i32,
        y_ptr: i32,
        y_size: i32,
    },

    /// Add two floats
    FloatAdd {
        x_ptr: i32,
        x_size: i32,
        y_ptr: i32,
        y_size: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Subtract two floats
    FloatSubtract {
        x_ptr: i32,
        x_size: i32,
        y_ptr: i32,
        y_size: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Multiply two floats
    FloatMultiply {
        x_ptr: i32,
        x_size: i32,
        y_ptr: i32,
        y_size: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Divide two floats
    FloatDivide {
        x_ptr: i32,
        x_size: i32,
        y_ptr: i32,
        y_size: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Compute nth root of a float
    FloatRoot {
        x_ptr: i32,
        x_size: i32,
        n: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Raise float to an integer power
    FloatPower {
        x_ptr: i32,
        x_size: i32,
        n: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },

    /// Compute natural logarithm of a float
    FloatLog {
        x_ptr: i32,
        x_size: i32,
        out_ptr: i32,
        out_size: i32,
        rounding: i32,
    },
}

impl HostFunction {
    /// Returns the name of the host function as it appears in WASM imports
    pub fn name(&self) -> &'static str {
        match self {
            Self::GetLedgerSqn => "get_ledger_sqn",
            Self::GetParentLedgerTime => "get_parent_ledger_time",
            Self::GetParentLedgerHash { .. } => "get_parent_ledger_hash",
            Self::GetBaseFee => "get_base_fee",
            Self::IsAmendmentEnabled { .. } => "amendment_enabled",
            Self::CacheLedgerObj { .. } => "cache_ledger_obj",
            Self::GetTxField { .. } => "get_tx_field",
            Self::GetCurrentLedgerObjField { .. } => "get_current_ledger_obj_field",
            Self::GetLedgerObjField { .. } => "get_ledger_obj_field",
            Self::GetTxNestedField { .. } => "get_tx_nested_field",
            Self::GetCurrentLedgerObjNestedField { .. } => "get_current_ledger_obj_nested_field",
            Self::GetLedgerObjNestedField { .. } => "get_ledger_obj_nested_field",
            Self::GetTxArrayLen { .. } => "get_tx_array_len",
            Self::GetCurrentLedgerObjArrayLen { .. } => "get_current_ledger_obj_array_len",
            Self::GetLedgerObjArrayLen { .. } => "get_ledger_obj_array_len",
            Self::GetTxNestedArrayLen { .. } => "get_tx_nested_array_len",
            Self::GetCurrentLedgerObjNestedArrayLen { .. } => "get_current_ledger_obj_nested_array_len",
            Self::GetLedgerObjNestedArrayLen { .. } => "get_ledger_obj_nested_array_len",
            Self::UpdateData { .. } => "update_data",
            Self::CheckSignature { .. } => "check_sig",
            Self::ComputeSha512HalfHash { .. } => "compute_sha512_half",
            Self::AccountKeylet { .. } => "account_keylet",
            Self::AmmKeylet { .. } => "amm_keylet",
            Self::CheckKeylet { .. } => "check_keylet",
            Self::CredentialKeylet { .. } => "credential_keylet",
            Self::DelegateKeylet { .. } => "delegate_keylet",
            Self::DepositPreauthKeylet { .. } => "deposit_preauth_keylet",
            Self::DidKeylet { .. } => "did_keylet",
            Self::EscrowKeylet { .. } => "escrow_keylet",
            Self::LineKeylet { .. } => "line_keylet",
            Self::MptIssuanceKeylet { .. } => "mpt_issuance_keylet",
            Self::MptokenKeylet { .. } => "mptoken_keylet",
            Self::NftOfferKeylet { .. } => "nft_offer_keylet",
            Self::OfferKeylet { .. } => "offer_keylet",
            Self::OracleKeylet { .. } => "oracle_keylet",
            Self::PaychanKeylet { .. } => "paychan_keylet",
            Self::PermissionedDomainKeylet { .. } => "permissioned_domain_keylet",
            Self::SignersKeylet { .. } => "signers_keylet",
            Self::TicketKeylet { .. } => "ticket_keylet",
            Self::VaultKeylet { .. } => "vault_keylet",
            Self::GetNFT { .. } => "get_nft",
            Self::GetNFTIssuer { .. } => "get_nft_issuer",
            Self::GetNFTTaxon { .. } => "get_nft_taxon",
            Self::GetNFTFlags { .. } => "get_nft_flags",
            Self::GetNFTTransferFee { .. } => "get_nft_transfer_fee",
            Self::GetNFTSerial { .. } => "get_nft_serial",
            Self::Trace { .. } => "trace",
            Self::TraceNum { .. } => "trace_num",
            Self::TraceAccount { .. } => "trace_account",
            Self::TraceFloat { .. } => "trace_opaque_float",
            Self::TraceAmount { .. } => "trace_amount",
            Self::FloatFromInt { .. } => "float_from_int",
            Self::FloatFromUint { .. } => "float_from_uint",
            Self::FloatSet { .. } => "float_set",
            Self::FloatCompare { .. } => "float_compare",
            Self::FloatAdd { .. } => "float_add",
            Self::FloatSubtract { .. } => "float_subtract",
            Self::FloatMultiply { .. } => "float_multiply",
            Self::FloatDivide { .. } => "float_divide",
            Self::FloatRoot { .. } => "float_root",
            Self::FloatPower { .. } => "float_pow",
            Self::FloatLog { .. } => "float_log",
        }
    }

    /// Returns the function signature for WAT import declarations
    pub fn signature(&self) -> &'static str {
        match self {
            Self::GetLedgerSqn => "(func (result i32))",
            Self::GetParentLedgerTime => "(func (result i32))",
            Self::GetParentLedgerHash { .. } => "(func (param i32 i32) (result i32))",
            Self::GetBaseFee => "(func (result i32))",
            Self::IsAmendmentEnabled { .. } => "(func (param i32 i32) (result i32))",
            Self::CacheLedgerObj { .. } => "(func (param i32 i32 i32) (result i32))",
            Self::GetTxField { .. } => "(func (param i32 i32 i32) (result i32))",
            Self::GetCurrentLedgerObjField { .. } => "(func (param i32 i32 i32) (result i32))",
            Self::GetLedgerObjField { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::GetTxNestedField { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::GetCurrentLedgerObjNestedField { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::GetLedgerObjNestedField { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::GetTxArrayLen { .. } => "(func (param i32) (result i32))",
            Self::GetCurrentLedgerObjArrayLen { .. } => "(func (param i32) (result i32))",
            Self::GetLedgerObjArrayLen { .. } => "(func (param i32 i32) (result i32))",
            Self::GetTxNestedArrayLen { .. } => "(func (param i32 i32) (result i32))",
            Self::GetCurrentLedgerObjNestedArrayLen { .. } => "(func (param i32 i32) (result i32))",
            Self::GetLedgerObjNestedArrayLen { .. } => "(func (param i32 i32 i32) (result i32))",
            Self::UpdateData { .. } => "(func (param i32 i32) (result i32))",
            Self::CheckSignature { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::ComputeSha512HalfHash { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::AccountKeylet { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::AmmKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::CheckKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::CredentialKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::DelegateKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::DepositPreauthKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::DidKeylet { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::EscrowKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::LineKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::MptIssuanceKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::MptokenKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::NftOfferKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::OfferKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::OracleKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::PaychanKeylet { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::PermissionedDomainKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::SignersKeylet { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::TicketKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::VaultKeylet { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::GetNFT { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::GetNFTIssuer { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::GetNFTTaxon { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::GetNFTFlags { .. } => "(func (param i32 i32) (result i32))",
            Self::GetNFTTransferFee { .. } => "(func (param i32 i32) (result i32))",
            Self::GetNFTSerial { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::Trace { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::TraceNum { .. } => "(func (param i32 i32 i64) (result i32))",
            Self::TraceAccount { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::TraceFloat { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::TraceAmount { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::FloatFromInt { .. } => "(func (param i64 i32 i32 i32) (result i32))",
            Self::FloatFromUint { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
            Self::FloatSet { .. } => "(func (param i32 i64 i32 i32 i32) (result i32))",
            Self::FloatCompare { .. } => "(func (param i32 i32 i32 i32) (result i32))",
            Self::FloatAdd { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::FloatSubtract { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::FloatMultiply { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::FloatDivide { .. } => "(func (param i32 i32 i32 i32 i32 i32 i32) (result i32))",
            Self::FloatRoot { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::FloatPower { .. } => "(func (param i32 i32 i32 i32 i32 i32) (result i32))",
            Self::FloatLog { .. } => "(func (param i32 i32 i32 i32 i32) (result i32))",
        }
    }

    /// Renders a WAT call expression for this host function
    pub fn render_call(&self) -> String {
        match self {
            Self::GetLedgerSqn => format!("(call $get_ledger_sqn)\ndrop"),
            Self::GetParentLedgerTime => format!("(call $get_parent_ledger_time)\ndrop"),
            Self::GetParentLedgerHash { out_ptr, out_size } => {
                format!("(call $get_parent_ledger_hash (i32.const {}) (i32.const {}))\ndrop", out_ptr, out_size)
            }
            Self::GetBaseFee => format!("(call $get_base_fee)\ndrop"),
            Self::IsAmendmentEnabled { amendment_ptr, amendment_size } => {
                format!("(call $amendment_enabled (i32.const {}) (i32.const {}))\ndrop", amendment_ptr, amendment_size)
            }
            Self::CacheLedgerObj { id_ptr, id_size, cache } => {
                format!("(call $cache_ledger_obj (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", id_ptr, id_size, cache)
            }
            Self::GetTxField { field_code, out_ptr, out_size } => {
                format!("(call $get_tx_field (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", field_code, out_ptr, out_size)
            }
            Self::GetCurrentLedgerObjField { field_code, out_ptr, out_size } => {
                format!("(call $get_current_ledger_obj_field (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", field_code, out_ptr, out_size)
            }
            Self::GetLedgerObjField { cache, field_code, out_ptr, out_size } => {
                format!("(call $get_ledger_obj_field (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", cache, field_code, out_ptr, out_size)
            }
            Self::GetTxNestedField { locator_ptr, locator_size, out_ptr, out_size } => {
                format!("(call $get_tx_nested_field (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", locator_ptr, locator_size, out_ptr, out_size)
            }
            Self::GetCurrentLedgerObjNestedField { locator_ptr, locator_size, out_ptr, out_size } => {
                format!("(call $get_current_ledger_obj_nested_field (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", locator_ptr, locator_size, out_ptr, out_size)
            }
            Self::GetLedgerObjNestedField { cache, locator_ptr, locator_size, out_ptr, out_size } => {
                format!("(call $get_ledger_obj_nested_field (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", cache, locator_ptr, locator_size, out_ptr, out_size)
            }
            Self::GetTxArrayLen { field_code } => {
                format!("(call $get_tx_array_len (i32.const {}))\ndrop", field_code)
            }
            Self::GetCurrentLedgerObjArrayLen { field_code } => {
                format!("(call $get_current_ledger_obj_array_len (i32.const {}))\ndrop", field_code)
            }
            Self::GetLedgerObjArrayLen { cache, field_code } => {
                format!("(call $get_ledger_obj_array_len (i32.const {}) (i32.const {}))\ndrop", cache, field_code)
            }
            Self::GetTxNestedArrayLen { locator_ptr, locator_size } => {
                format!("(call $get_tx_nested_array_len (i32.const {}) (i32.const {}))\ndrop", locator_ptr, locator_size)
            }
            Self::GetCurrentLedgerObjNestedArrayLen { locator_ptr, locator_size } => {
                format!("(call $get_current_ledger_obj_nested_array_len (i32.const {}) (i32.const {}))\ndrop", locator_ptr, locator_size)
            }
            Self::GetLedgerObjNestedArrayLen { cache, locator_ptr, locator_size } => {
                format!("(call $get_ledger_obj_nested_array_len (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", cache, locator_ptr, locator_size)
            }
            Self::UpdateData { data_ptr, data_size } => {
                format!("(call $update_data (i32.const {}) (i32.const {}))\ndrop", data_ptr, data_size)
            }
            Self::CheckSignature { msg_ptr, msg_size, sig_ptr, sig_size, key_ptr, key_size } => {
                format!("(call $check_sig (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", msg_ptr, msg_size, sig_ptr, sig_size, key_ptr, key_size)
            }
            Self::ComputeSha512HalfHash { data_ptr, data_size, out_ptr, out_size } => {
                format!("(call $compute_sha512_half (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", data_ptr, data_size, out_ptr, out_size)
            }
            Self::AccountKeylet { acc_ptr, acc_size, out_ptr, out_size } => {
                format!("(call $account_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, out_ptr, out_size)
            }
            Self::AmmKeylet { asset1_ptr, asset1_size, asset2_ptr, asset2_size, out_ptr, out_size } => {
                format!("(call $amm_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", asset1_ptr, asset1_size, asset2_ptr, asset2_size, out_ptr, out_size)
            }
            Self::CheckKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $check_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::CredentialKeylet { subj_ptr, subj_size, iss_ptr, iss_size, cred_ptr, cred_size, out_ptr, out_size } => {
                format!("(call $credential_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", subj_ptr, subj_size, iss_ptr, iss_size, cred_ptr, cred_size, out_ptr, out_size)
            }
            Self::DelegateKeylet { acc_ptr, acc_size, auth_ptr, auth_size, out_ptr, out_size } => {
                format!("(call $delegate_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, auth_ptr, auth_size, out_ptr, out_size)
            }
            Self::DepositPreauthKeylet { acc_ptr, acc_size, auth_ptr, auth_size, out_ptr, out_size } => {
                format!("(call $deposit_preauth_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, auth_ptr, auth_size, out_ptr, out_size)
            }
            Self::DidKeylet { acc_ptr, acc_size, out_ptr, out_size } => {
                format!("(call $did_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, out_ptr, out_size)
            }
            Self::EscrowKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $escrow_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::LineKeylet { acc1_ptr, acc1_size, acc2_ptr, acc2_size, curr_ptr, curr_size, out_ptr, out_size } => {
                format!("(call $line_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc1_ptr, acc1_size, acc2_ptr, acc2_size, curr_ptr, curr_size, out_ptr, out_size)
            }
            Self::MptIssuanceKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $mpt_issuance_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::MptokenKeylet { mptid_ptr, mptid_size, holder_ptr, holder_size, out_ptr, out_size } => {
                format!("(call $mptoken_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", mptid_ptr, mptid_size, holder_ptr, holder_size, out_ptr, out_size)
            }
            Self::NftOfferKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $nft_offer_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::OfferKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $offer_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::OracleKeylet { acc_ptr, acc_size, doc_id, out_ptr, out_size } => {
                format!("(call $oracle_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, doc_id, out_ptr, out_size)
            }
            Self::PaychanKeylet { acc_ptr, acc_size, dest_ptr, dest_size, seq, out_ptr, out_size } => {
                format!("(call $paychan_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, dest_ptr, dest_size, seq, out_ptr, out_size)
            }
            Self::PermissionedDomainKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $permissioned_domain_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::SignersKeylet { acc_ptr, acc_size, out_ptr, out_size } => {
                format!("(call $signers_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, out_ptr, out_size)
            }
            Self::TicketKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $ticket_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::VaultKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                format!("(call $vault_keylet (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, seq, out_ptr, out_size)
            }
            Self::GetNFT { acc_ptr, acc_size, nftid_ptr, nftid_size, out_ptr, out_size } => {
                format!("(call $get_nft (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", acc_ptr, acc_size, nftid_ptr, nftid_size, out_ptr, out_size)
            }
            Self::GetNFTIssuer { nftid_ptr, nftid_size, out_ptr, out_size } => {
                format!("(call $get_nft_issuer (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", nftid_ptr, nftid_size, out_ptr, out_size)
            }
            Self::GetNFTTaxon { nftid_ptr, nftid_size, out_ptr, out_size } => {
                format!("(call $get_nft_taxon (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", nftid_ptr, nftid_size, out_ptr, out_size)
            }
            Self::GetNFTFlags { nftid_ptr, nftid_size } => {
                format!("(call $get_nft_flags (i32.const {}) (i32.const {}))\ndrop", nftid_ptr, nftid_size)
            }
            Self::GetNFTTransferFee { nftid_ptr, nftid_size } => {
                format!("(call $get_nft_transfer_fee (i32.const {}) (i32.const {}))\ndrop", nftid_ptr, nftid_size)
            }
            Self::GetNFTSerial { nftid_ptr, nftid_size, out_ptr, out_size } => {
                format!("(call $get_nft_serial (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", nftid_ptr, nftid_size, out_ptr, out_size)
            }
            Self::Trace { msg_ptr, msg_size, data_ptr, data_size, as_hex } => {
                format!("(call $trace (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", msg_ptr, msg_size, data_ptr, data_size, as_hex)
            }
            Self::TraceNum { msg_ptr, msg_size, number } => {
                format!("(call $trace_num (i32.const {}) (i32.const {}) (i64.const {}))\ndrop", msg_ptr, msg_size, number)
            }
            Self::TraceAccount { msg_ptr, msg_size, acc_ptr, acc_size } => {
                format!("(call $trace_account (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", msg_ptr, msg_size, acc_ptr, acc_size)
            }
            Self::TraceFloat { msg_ptr, msg_size, float_ptr, float_size } => {
                format!("(call $trace_opaque_float (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", msg_ptr, msg_size, float_ptr, float_size)
            }
            Self::TraceAmount { msg_ptr, msg_size, amount_ptr, amount_size } => {
                format!("(call $trace_amount (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", msg_ptr, msg_size, amount_ptr, amount_size)
            }
            Self::FloatFromInt { x, out_ptr, out_size, rounding } => {
                format!("(call $float_from_int (i64.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x, out_ptr, out_size, rounding)
            }
            Self::FloatFromUint { x_ptr, x_size, out_ptr, out_size, rounding } => {
                format!("(call $float_from_uint (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, out_ptr, out_size, rounding)
            }
            Self::FloatSet { exp, mant, out_ptr, out_size, rounding } => {
                format!("(call $float_set (i32.const {}) (i64.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", exp, mant, out_ptr, out_size, rounding)
            }
            Self::FloatCompare { x_ptr, x_size, y_ptr, y_size } => {
                format!("(call $float_compare (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, y_ptr, y_size)
            }
            Self::FloatAdd { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                format!("(call $float_add (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding)
            }
            Self::FloatSubtract { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                format!("(call $float_subtract (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding)
            }
            Self::FloatMultiply { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                format!("(call $float_multiply (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding)
            }
            Self::FloatDivide { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                format!("(call $float_divide (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding)
            }
            Self::FloatRoot { x_ptr, x_size, n, out_ptr, out_size, rounding } => {
                format!("(call $float_root (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, n, out_ptr, out_size, rounding)
            }
            Self::FloatPower { x_ptr, x_size, n, out_ptr, out_size, rounding } => {
                format!("(call $float_pow (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, n, out_ptr, out_size, rounding)
            }
            Self::FloatLog { x_ptr, x_size, out_ptr, out_size, rounding } => {
                format!("(call $float_log (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}) (i32.const {}))\ndrop", x_ptr, x_size, out_ptr, out_size, rounding)
            }
        }
    }
}

#[derive(Debug, Clone, autarkie::Grammar, serde::Serialize, serde::Deserialize)]
pub struct FuzzData {
    memory: Vec<u32>,
    calls: Vec<HostFunction>
}

impl FuzzData {
    /// Returns the size of memory in bytes
    pub fn memory_size_bytes(&self) -> usize {
        self.memory.len() * 4
    }

    /// Total memory size in bytes including the seed region.
    fn total_memory_bytes(&self) -> usize {
        known_ids::SEED_SIZE + self.memory_size_bytes()
    }

    /// Normalizes a pointer value to be within valid memory bounds
    fn normalize_ptr(&self, ptr: i32) -> i32 {
        let mem_size = self.total_memory_bytes() as i32;
        if mem_size == 0 {
            0
        } else {
            let ptr_abs = ptr.abs();
            ptr_abs % mem_size
        }
    }

    /// Normalizes a size parameter to prevent reads beyond memory bounds
    fn normalize_size(&self, ptr: i32, size: i32) -> i32 {
        let mem_size = self.total_memory_bytes() as i32;
        if mem_size == 0 {
            0
        } else {
            let normalized_ptr = self.normalize_ptr(ptr);
            let max_size = mem_size - normalized_ptr;
            size.abs().min(max_size).max(0)
        }
    }

    /// Normalizes all pointers to be within valid memory bounds (modifies in place)
    pub fn normalize(&mut self) {
        let calls = std::mem::take(&mut self.calls);
        self.calls = calls.into_iter().map(|call| {
            match call {
                HostFunction::GetParentLedgerHash { out_ptr, out_size } => {
                    let norm_ptr = self.normalize_ptr(out_ptr);
                    let norm_size = self.normalize_size(norm_ptr, out_size);
                    HostFunction::GetParentLedgerHash { out_ptr: norm_ptr, out_size: norm_size }
                }
                HostFunction::IsAmendmentEnabled { amendment_ptr, amendment_size } => {
                    let norm_ptr = self.normalize_ptr(amendment_ptr);
                    let norm_size = self.normalize_size(norm_ptr, amendment_size);
                    HostFunction::IsAmendmentEnabled { amendment_ptr: norm_ptr, amendment_size: norm_size }
                }
                HostFunction::CacheLedgerObj { id_ptr, id_size, cache } => {
                    let norm_ptr = self.normalize_ptr(id_ptr);
                    let norm_size = self.normalize_size(norm_ptr, id_size);
                    HostFunction::CacheLedgerObj { id_ptr: norm_ptr, id_size: norm_size, cache }
                }
                HostFunction::GetTxField { field_code, out_ptr, out_size } => {
                    let norm_ptr = self.normalize_ptr(out_ptr);
                    let norm_size = self.normalize_size(norm_ptr, out_size);
                    HostFunction::GetTxField { field_code, out_ptr: norm_ptr, out_size: norm_size }
                }
                HostFunction::GetCurrentLedgerObjField { field_code, out_ptr, out_size } => {
                    let norm_ptr = self.normalize_ptr(out_ptr);
                    let norm_size = self.normalize_size(norm_ptr, out_size);
                    HostFunction::GetCurrentLedgerObjField { field_code, out_ptr: norm_ptr, out_size: norm_size }
                }
                HostFunction::GetLedgerObjField { cache, field_code, out_ptr, out_size } => {
                    let norm_ptr = self.normalize_ptr(out_ptr);
                    let norm_size = self.normalize_size(norm_ptr, out_size);
                    HostFunction::GetLedgerObjField { cache, field_code, out_ptr: norm_ptr, out_size: norm_size }
                }
                HostFunction::GetTxNestedField { locator_ptr, locator_size, out_ptr, out_size } => {
                    let norm_loc_ptr = self.normalize_ptr(locator_ptr);
                    let norm_loc_size = self.normalize_size(norm_loc_ptr, locator_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetTxNestedField {
                        locator_ptr: norm_loc_ptr, locator_size: norm_loc_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetCurrentLedgerObjNestedField { locator_ptr, locator_size, out_ptr, out_size } => {
                    let norm_loc_ptr = self.normalize_ptr(locator_ptr);
                    let norm_loc_size = self.normalize_size(norm_loc_ptr, locator_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetCurrentLedgerObjNestedField {
                        locator_ptr: norm_loc_ptr, locator_size: norm_loc_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetLedgerObjNestedField { cache, locator_ptr, locator_size, out_ptr, out_size } => {
                    let norm_loc_ptr = self.normalize_ptr(locator_ptr);
                    let norm_loc_size = self.normalize_size(norm_loc_ptr, locator_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetLedgerObjNestedField {
                        cache,
                        locator_ptr: norm_loc_ptr, locator_size: norm_loc_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetTxNestedArrayLen { locator_ptr, locator_size } => {
                    let norm_ptr = self.normalize_ptr(locator_ptr);
                    let norm_size = self.normalize_size(norm_ptr, locator_size);
                    HostFunction::GetTxNestedArrayLen { locator_ptr: norm_ptr, locator_size: norm_size }
                }
                HostFunction::GetCurrentLedgerObjNestedArrayLen { locator_ptr, locator_size } => {
                    let norm_ptr = self.normalize_ptr(locator_ptr);
                    let norm_size = self.normalize_size(norm_ptr, locator_size);
                    HostFunction::GetCurrentLedgerObjNestedArrayLen { locator_ptr: norm_ptr, locator_size: norm_size }
                }
                HostFunction::GetLedgerObjNestedArrayLen { cache, locator_ptr, locator_size } => {
                    let norm_ptr = self.normalize_ptr(locator_ptr);
                    let norm_size = self.normalize_size(norm_ptr, locator_size);
                    HostFunction::GetLedgerObjNestedArrayLen { cache, locator_ptr: norm_ptr, locator_size: norm_size }
                }
                HostFunction::UpdateData { data_ptr, data_size } => {
                    let norm_ptr = self.normalize_ptr(data_ptr);
                    let norm_size = self.normalize_size(norm_ptr, data_size);
                    HostFunction::UpdateData { data_ptr: norm_ptr, data_size: norm_size }
                }
                HostFunction::CheckSignature { msg_ptr, msg_size, sig_ptr, sig_size, key_ptr, key_size } => {
                    let norm_msg_ptr = self.normalize_ptr(msg_ptr);
                    let norm_msg_size = self.normalize_size(norm_msg_ptr, msg_size);
                    let norm_sig_ptr = self.normalize_ptr(sig_ptr);
                    let norm_sig_size = self.normalize_size(norm_sig_ptr, sig_size);
                    let norm_key_ptr = self.normalize_ptr(key_ptr);
                    let norm_key_size = self.normalize_size(norm_key_ptr, key_size);
                    HostFunction::CheckSignature {
                        msg_ptr: norm_msg_ptr, msg_size: norm_msg_size,
                        sig_ptr: norm_sig_ptr, sig_size: norm_sig_size,
                        key_ptr: norm_key_ptr, key_size: norm_key_size
                    }
                }
                HostFunction::ComputeSha512HalfHash { data_ptr, data_size, out_ptr, out_size } => {
                    let norm_data_ptr = self.normalize_ptr(data_ptr);
                    let norm_data_size = self.normalize_size(norm_data_ptr, data_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::ComputeSha512HalfHash {
                        data_ptr: norm_data_ptr, data_size: norm_data_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::AccountKeylet { acc_ptr, acc_size, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::AccountKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::AmmKeylet { asset1_ptr, asset1_size, asset2_ptr, asset2_size, out_ptr, out_size } => {
                    let norm_a1_ptr = self.normalize_ptr(asset1_ptr);
                    let norm_a1_size = self.normalize_size(norm_a1_ptr, asset1_size);
                    let norm_a2_ptr = self.normalize_ptr(asset2_ptr);
                    let norm_a2_size = self.normalize_size(norm_a2_ptr, asset2_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::AmmKeylet {
                        asset1_ptr: norm_a1_ptr, asset1_size: norm_a1_size,
                        asset2_ptr: norm_a2_ptr, asset2_size: norm_a2_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::CheckKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::CheckKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::CredentialKeylet { subj_ptr, subj_size, iss_ptr, iss_size, cred_ptr, cred_size, out_ptr, out_size } => {
                    let norm_subj_ptr = self.normalize_ptr(subj_ptr);
                    let norm_subj_size = self.normalize_size(norm_subj_ptr, subj_size);
                    let norm_iss_ptr = self.normalize_ptr(iss_ptr);
                    let norm_iss_size = self.normalize_size(norm_iss_ptr, iss_size);
                    let norm_cred_ptr = self.normalize_ptr(cred_ptr);
                    let norm_cred_size = self.normalize_size(norm_cred_ptr, cred_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::CredentialKeylet {
                        subj_ptr: norm_subj_ptr, subj_size: norm_subj_size,
                        iss_ptr: norm_iss_ptr, iss_size: norm_iss_size,
                        cred_ptr: norm_cred_ptr, cred_size: norm_cred_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::DelegateKeylet { acc_ptr, acc_size, auth_ptr, auth_size, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_auth_ptr = self.normalize_ptr(auth_ptr);
                    let norm_auth_size = self.normalize_size(norm_auth_ptr, auth_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::DelegateKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        auth_ptr: norm_auth_ptr, auth_size: norm_auth_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::DepositPreauthKeylet { acc_ptr, acc_size, auth_ptr, auth_size, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_auth_ptr = self.normalize_ptr(auth_ptr);
                    let norm_auth_size = self.normalize_size(norm_auth_ptr, auth_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::DepositPreauthKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        auth_ptr: norm_auth_ptr, auth_size: norm_auth_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::DidKeylet { acc_ptr, acc_size, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::DidKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::EscrowKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::EscrowKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::LineKeylet { acc1_ptr, acc1_size, acc2_ptr, acc2_size, curr_ptr, curr_size, out_ptr, out_size } => {
                    let norm_acc1_ptr = self.normalize_ptr(acc1_ptr);
                    let norm_acc1_size = self.normalize_size(norm_acc1_ptr, acc1_size);
                    let norm_acc2_ptr = self.normalize_ptr(acc2_ptr);
                    let norm_acc2_size = self.normalize_size(norm_acc2_ptr, acc2_size);
                    let norm_curr_ptr = self.normalize_ptr(curr_ptr);
                    let norm_curr_size = self.normalize_size(norm_curr_ptr, curr_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::LineKeylet {
                        acc1_ptr: norm_acc1_ptr, acc1_size: norm_acc1_size,
                        acc2_ptr: norm_acc2_ptr, acc2_size: norm_acc2_size,
                        curr_ptr: norm_curr_ptr, curr_size: norm_curr_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::MptIssuanceKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::MptIssuanceKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::MptokenKeylet { mptid_ptr, mptid_size, holder_ptr, holder_size, out_ptr, out_size } => {
                    let norm_mptid_ptr = self.normalize_ptr(mptid_ptr);
                    let norm_mptid_size = self.normalize_size(norm_mptid_ptr, mptid_size);
                    let norm_holder_ptr = self.normalize_ptr(holder_ptr);
                    let norm_holder_size = self.normalize_size(norm_holder_ptr, holder_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::MptokenKeylet {
                        mptid_ptr: norm_mptid_ptr, mptid_size: norm_mptid_size,
                        holder_ptr: norm_holder_ptr, holder_size: norm_holder_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::NftOfferKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::NftOfferKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::OfferKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::OfferKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::OracleKeylet { acc_ptr, acc_size, doc_id, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::OracleKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, doc_id,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::PaychanKeylet { acc_ptr, acc_size, dest_ptr, dest_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_dest_ptr = self.normalize_ptr(dest_ptr);
                    let norm_dest_size = self.normalize_size(norm_dest_ptr, dest_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::PaychanKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        dest_ptr: norm_dest_ptr, dest_size: norm_dest_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::PermissionedDomainKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::PermissionedDomainKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::SignersKeylet { acc_ptr, acc_size, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::SignersKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::TicketKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::TicketKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::VaultKeylet { acc_ptr, acc_size, seq, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::VaultKeylet {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size, seq,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetNFT { acc_ptr, acc_size, nftid_ptr, nftid_size, out_ptr, out_size } => {
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    let norm_nftid_ptr = self.normalize_ptr(nftid_ptr);
                    let norm_nftid_size = self.normalize_size(norm_nftid_ptr, nftid_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetNFT {
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size,
                        nftid_ptr: norm_nftid_ptr, nftid_size: norm_nftid_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetNFTIssuer { nftid_ptr, nftid_size, out_ptr, out_size } => {
                    let norm_nftid_ptr = self.normalize_ptr(nftid_ptr);
                    let norm_nftid_size = self.normalize_size(norm_nftid_ptr, nftid_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetNFTIssuer {
                        nftid_ptr: norm_nftid_ptr, nftid_size: norm_nftid_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetNFTTaxon { nftid_ptr, nftid_size, out_ptr, out_size } => {
                    let norm_nftid_ptr = self.normalize_ptr(nftid_ptr);
                    let norm_nftid_size = self.normalize_size(norm_nftid_ptr, nftid_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetNFTTaxon {
                        nftid_ptr: norm_nftid_ptr, nftid_size: norm_nftid_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::GetNFTFlags { nftid_ptr, nftid_size } => {
                    let norm_ptr = self.normalize_ptr(nftid_ptr);
                    let norm_size = self.normalize_size(norm_ptr, nftid_size);
                    HostFunction::GetNFTFlags { nftid_ptr: norm_ptr, nftid_size: norm_size }
                }
                HostFunction::GetNFTTransferFee { nftid_ptr, nftid_size } => {
                    let norm_ptr = self.normalize_ptr(nftid_ptr);
                    let norm_size = self.normalize_size(norm_ptr, nftid_size);
                    HostFunction::GetNFTTransferFee { nftid_ptr: norm_ptr, nftid_size: norm_size }
                }
                HostFunction::GetNFTSerial { nftid_ptr, nftid_size, out_ptr, out_size } => {
                    let norm_nftid_ptr = self.normalize_ptr(nftid_ptr);
                    let norm_nftid_size = self.normalize_size(norm_nftid_ptr, nftid_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::GetNFTSerial {
                        nftid_ptr: norm_nftid_ptr, nftid_size: norm_nftid_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size
                    }
                }
                HostFunction::Trace { msg_ptr, msg_size, data_ptr, data_size, as_hex } => {
                    let norm_msg_ptr = self.normalize_ptr(msg_ptr);
                    let norm_msg_size = self.normalize_size(norm_msg_ptr, msg_size);
                    let norm_data_ptr = self.normalize_ptr(data_ptr);
                    let norm_data_size = self.normalize_size(norm_data_ptr, data_size);
                    HostFunction::Trace {
                        msg_ptr: norm_msg_ptr, msg_size: norm_msg_size,
                        data_ptr: norm_data_ptr, data_size: norm_data_size, as_hex
                    }
                }
                HostFunction::TraceNum { msg_ptr, msg_size, number } => {
                    let norm_ptr = self.normalize_ptr(msg_ptr);
                    let norm_size = self.normalize_size(norm_ptr, msg_size);
                    HostFunction::TraceNum { msg_ptr: norm_ptr, msg_size: norm_size, number }
                }
                HostFunction::TraceAccount { msg_ptr, msg_size, acc_ptr, acc_size } => {
                    let norm_msg_ptr = self.normalize_ptr(msg_ptr);
                    let norm_msg_size = self.normalize_size(norm_msg_ptr, msg_size);
                    let norm_acc_ptr = self.normalize_ptr(acc_ptr);
                    let norm_acc_size = self.normalize_size(norm_acc_ptr, acc_size);
                    HostFunction::TraceAccount {
                        msg_ptr: norm_msg_ptr, msg_size: norm_msg_size,
                        acc_ptr: norm_acc_ptr, acc_size: norm_acc_size
                    }
                }
                HostFunction::TraceFloat { msg_ptr, msg_size, float_ptr, float_size } => {
                    let norm_msg_ptr = self.normalize_ptr(msg_ptr);
                    let norm_msg_size = self.normalize_size(norm_msg_ptr, msg_size);
                    let norm_float_ptr = self.normalize_ptr(float_ptr);
                    let norm_float_size = self.normalize_size(norm_float_ptr, float_size);
                    HostFunction::TraceFloat {
                        msg_ptr: norm_msg_ptr, msg_size: norm_msg_size,
                        float_ptr: norm_float_ptr, float_size: norm_float_size
                    }
                }
                HostFunction::TraceAmount { msg_ptr, msg_size, amount_ptr, amount_size } => {
                    let norm_msg_ptr = self.normalize_ptr(msg_ptr);
                    let norm_msg_size = self.normalize_size(norm_msg_ptr, msg_size);
                    let norm_amount_ptr = self.normalize_ptr(amount_ptr);
                    let norm_amount_size = self.normalize_size(norm_amount_ptr, amount_size);
                    HostFunction::TraceAmount {
                        msg_ptr: norm_msg_ptr, msg_size: norm_msg_size,
                        amount_ptr: norm_amount_ptr, amount_size: norm_amount_size
                    }
                }
                HostFunction::FloatFromInt { x, out_ptr, out_size, rounding } => {
                    let norm_ptr = self.normalize_ptr(out_ptr);
                    let norm_size = self.normalize_size(norm_ptr, out_size);
                    HostFunction::FloatFromInt { x, out_ptr: norm_ptr, out_size: norm_size, rounding }
                }
                HostFunction::FloatFromUint { x_ptr, x_size, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatFromUint {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatSet { exp, mant, out_ptr, out_size, rounding } => {
                    let norm_ptr = self.normalize_ptr(out_ptr);
                    let norm_size = self.normalize_size(norm_ptr, out_size);
                    HostFunction::FloatSet { exp, mant, out_ptr: norm_ptr, out_size: norm_size, rounding }
                }
                HostFunction::FloatCompare { x_ptr, x_size, y_ptr, y_size } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_y_ptr = self.normalize_ptr(y_ptr);
                    let norm_y_size = self.normalize_size(norm_y_ptr, y_size);
                    HostFunction::FloatCompare {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        y_ptr: norm_y_ptr, y_size: norm_y_size
                    }
                }
                HostFunction::FloatAdd { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_y_ptr = self.normalize_ptr(y_ptr);
                    let norm_y_size = self.normalize_size(norm_y_ptr, y_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatAdd {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        y_ptr: norm_y_ptr, y_size: norm_y_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatSubtract { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_y_ptr = self.normalize_ptr(y_ptr);
                    let norm_y_size = self.normalize_size(norm_y_ptr, y_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatSubtract {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        y_ptr: norm_y_ptr, y_size: norm_y_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatMultiply { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_y_ptr = self.normalize_ptr(y_ptr);
                    let norm_y_size = self.normalize_size(norm_y_ptr, y_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatMultiply {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        y_ptr: norm_y_ptr, y_size: norm_y_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatDivide { x_ptr, x_size, y_ptr, y_size, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_y_ptr = self.normalize_ptr(y_ptr);
                    let norm_y_size = self.normalize_size(norm_y_ptr, y_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatDivide {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        y_ptr: norm_y_ptr, y_size: norm_y_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatRoot { x_ptr, x_size, n, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatRoot {
                        x_ptr: norm_x_ptr, x_size: norm_x_size, n,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatPower { x_ptr, x_size, n, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatPower {
                        x_ptr: norm_x_ptr, x_size: norm_x_size, n,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                HostFunction::FloatLog { x_ptr, x_size, out_ptr, out_size, rounding } => {
                    let norm_x_ptr = self.normalize_ptr(x_ptr);
                    let norm_x_size = self.normalize_size(norm_x_ptr, x_size);
                    let norm_out_ptr = self.normalize_ptr(out_ptr);
                    let norm_out_size = self.normalize_size(norm_out_ptr, out_size);
                    HostFunction::FloatLog {
                        x_ptr: norm_x_ptr, x_size: norm_x_size,
                        out_ptr: norm_out_ptr, out_size: norm_out_size, rounding
                    }
                }
                // Functions with no pointers remain unchanged
                HostFunction::GetLedgerSqn => HostFunction::GetLedgerSqn,
                HostFunction::GetParentLedgerTime => HostFunction::GetParentLedgerTime,
                HostFunction::GetBaseFee => HostFunction::GetBaseFee,
                HostFunction::GetTxArrayLen { field_code } => HostFunction::GetTxArrayLen { field_code },
                HostFunction::GetCurrentLedgerObjArrayLen { field_code } => HostFunction::GetCurrentLedgerObjArrayLen { field_code },
                HostFunction::GetLedgerObjArrayLen { cache, field_code } => HostFunction::GetLedgerObjArrayLen { cache, field_code },
            }
        }).collect();
    }

    /// Renders the FuzzData as a WAT (WebAssembly Text) module
    pub fn render(&self) -> String {
        use std::collections::HashSet;

        let mut wat = String::new();

        // Module header
        wat.push_str("(module\n");

        // Collect unique host functions to import (fuzzer calls + preamble)
        let mut imported_funcs: HashSet<&str> = HashSet::new();
        for call in &self.calls {
            imported_funcs.insert(call.name());
        }
        for &(name, _) in known_ids::PREAMBLE_IMPORTS {
            imported_funcs.insert(name);
        }

        // Generate import declarations
        wat.push_str("  ;; Import host functions\n");
        for func_name in imported_funcs.iter() {
            // Check preamble imports first (they have known signatures)
            if let Some(&(_, sig)) = known_ids::PREAMBLE_IMPORTS.iter().find(|&&(n, _)| n == *func_name) {
                wat.push_str(&format!("  (import \"env\" \"{}\" {})\n", func_name, sig));
            } else if let Some(call) = self.calls.iter().find(|c| c.name() == *func_name) {
                let sig = call.signature();
                let sig_with_name = sig.replace("(func ", &format!("(func ${} ", func_name));
                wat.push_str(&format!("  (import \"env\" \"{}\" {})\n", func_name, sig_with_name));
            }
        }
        wat.push_str("\n");

        // Memory declaration (seed region + fuzzer-generated data)
        let total_bytes = self.total_memory_bytes();
        let memory_pages = ((total_bytes + 65535) / 65536).max(1);
        wat.push_str(&format!("  ;; Memory (export for fuzzer access)\n"));
        wat.push_str(&format!("  (memory {} {})\n", memory_pages, memory_pages));
        wat.push_str("  (export \"memory\" (memory 0))\n\n");

        // Seed region: known account IDs and identifiers at offset 0
        // so that normalized pointers have a chance of hitting valid data.
        {
            let seed = known_ids::seed_bytes();
            wat.push_str("  ;; Seed: known account IDs and identifiers\n");
            wat.push_str(&format!("  (data (i32.const 0) \""));
            for b in &seed {
                wat.push_str(&format!("\\{:02x}", b));
            }
            wat.push_str("\")\n\n");
        }

        // Fuzzer-generated random memory (after the seed region)
        if !self.memory.is_empty() {
            wat.push_str("  ;; Fuzzer-generated memory\n");
            let mut offset = known_ids::SEED_SIZE;
            for &value in &self.memory {
                let bytes = value.to_le_bytes();
                wat.push_str(&format!(
                    "  (data (i32.const {}) \"\\{:02x}\\{:02x}\\{:02x}\\{:02x}\")\n",
                    offset, bytes[0], bytes[1], bytes[2], bytes[3]
                ));
                offset += 4;
            }
            wat.push_str("\n");
        }

        // Main function: preamble (cache real objects) + fuzzer-generated calls
        wat.push_str("  ;; Main fuzz function\n");
        wat.push_str("  (func (export \"fuzz\") (result i32)\n");

        // Preamble: compute keylets and populate cache slots 1-8
        wat.push_str(&known_ids::preamble_wat());

        // Fuzzer-generated random calls
        for call in &self.calls {
            wat.push_str("    ");
            wat.push_str(&call.render_call());
            wat.push_str("\n");
        }

        // Return i64 value
        wat.push_str("    (i32.const 0)\n");
        wat.push_str("  )\n");

        // Module footer
        wat.push_str(")\n");

        wat
    }
}

autarkie::fuzz_afl!(FuzzData, |data: &FuzzData| -> Vec<u8> {
    let mut data = data.clone();
    data.normalize();
    let rendered = data.render();
/*     println!("{}", rendered); */
    wabt::wat2wasm(rendered).unwrap()
});
