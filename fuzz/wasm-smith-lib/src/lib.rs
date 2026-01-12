use arbitrary::Unstructured;
use tree_sitter::{InputEdit, Language, Parser, Point};
use wasm_smith::{Config, InstructionKind, InstructionKinds, Module};

#[repr(C)]
pub struct Slice {
    pub ptr: *const u8,
    pub len: usize,
}

#[unsafe(no_mangle)]
unsafe fn fuzz_get_module(ptr: *const u8, len: usize) -> Slice {
    let config = Config {
        allow_floats: false,
        allow_start_export: true,
        available_imports: Some(wat::parse_str(r#"
(module
    ;; Ledger Information
    (import "env" "get_ledger_sqn" (func $get_ledger_sqn (result i32)))
    (import "env" "get_parent_ledger_time" (func $get_parent_ledger_time (result i32)))
    (import "env" "get_parent_ledger_hash" (func $get_parent_ledger_hash (param i32 i32) (result i32)))
    (import "env" "get_base_fee" (func $get_base_fee (result i32)))
    (import "env" "amendment_enabled" (func $amendment_enabled (param i32 i32) (result i32)))

    ;; Ledger Object Caching
    (import "env" "cache_ledger_obj" (func $cache_ledger_obj (param i32 i32 i32) (result i32)))

    ;; Field Accessors
    (import "env" "get_tx_field" (func $get_tx_field (param i32 i32 i32) (result i32)))
    (import "env" "get_current_ledger_obj_field" (func $get_current_ledger_obj_field (param i32 i32 i32) (result i32)))
    (import "env" "get_ledger_obj_field" (func $get_ledger_obj_field (param i32 i32 i32 i32) (result i32)))

    ;; Nested Field Accessors
    (import "env" "get_tx_nested_field" (func $get_tx_nested_field (param i32 i32 i32 i32) (result i32)))
    (import "env" "get_current_ledger_obj_nested_field" (func $get_current_ledger_obj_nested_field (param i32 i32 i32 i32) (result i32)))
    (import "env" "get_ledger_obj_nested_field" (func $get_ledger_obj_nested_field (param i32 i32 i32 i32 i32) (result i32)))

    ;; Array Length Functions
    (import "env" "get_tx_array_len" (func $get_tx_array_len (param i32) (result i32)))
    (import "env" "get_current_ledger_obj_array_len" (func $get_current_ledger_obj_array_len (param i32) (result i32)))
    (import "env" "get_ledger_obj_array_len" (func $get_ledger_obj_array_len (param i32 i32) (result i32)))
    (import "env" "get_tx_nested_array_len" (func $get_tx_nested_array_len (param i32 i32) (result i32)))
    (import "env" "get_current_ledger_obj_nested_array_len" (func $get_current_ledger_obj_nested_array_len (param i32 i32) (result i32)))
    (import "env" "get_ledger_obj_nested_array_len" (func $get_ledger_obj_nested_array_len (param i32 i32 i32) (result i32)))

    ;; Data Update
    (import "env" "update_data" (func $update_data (param i32 i32) (result i32)))

    ;; Cryptographic Functions
    (import "env" "check_sig" (func $check_sig (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "compute_sha512_half" (func $compute_sha512_half (param i32 i32 i32 i32) (result i32)))

    ;; Keylet Functions
    (import "env" "account_keylet" (func $account_keylet (param i32 i32 i32 i32) (result i32)))
    (import "env" "amm_keylet" (func $amm_keylet (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "check_keylet" (func $check_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "credential_keylet" (func $credential_keylet (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "did_keylet" (func $did_keylet (param i32 i32 i32 i32) (result i32)))
    (import "env" "delegate_keylet" (func $delegate_keylet (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "deposit_preauth_keylet" (func $deposit_preauth_keylet (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "escrow_keylet" (func $escrow_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "line_keylet" (func $line_keylet (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "mpt_issuance_keylet" (func $mpt_issuance_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "mptoken_keylet" (func $mptoken_keylet (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "nft_offer_keylet" (func $nft_offer_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "offer_keylet" (func $offer_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "oracle_keylet" (func $oracle_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "paychan_keylet" (func $paychan_keylet (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "permissioned_domain_keylet" (func $permissioned_domain_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "signers_keylet" (func $signers_keylet (param i32 i32 i32 i32) (result i32)))
    (import "env" "ticket_keylet" (func $ticket_keylet (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "vault_keylet" (func $vault_keylet (param i32 i32 i32 i32 i32) (result i32)))

    ;; NFT Functions
    (import "env" "get_nft" (func $get_nft (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "get_nft_issuer" (func $get_nft_issuer (param i32 i32 i32 i32) (result i32)))
    (import "env" "get_nft_taxon" (func $get_nft_taxon (param i32 i32 i32 i32) (result i32)))
    (import "env" "get_nft_flags" (func $get_nft_flags (param i32 i32) (result i32)))
    (import "env" "get_nft_transfer_fee" (func $get_nft_transfer_fee (param i32 i32) (result i32)))
    (import "env" "get_nft_serial" (func $get_nft_serial (param i32 i32 i32 i32) (result i32)))

    ;; Trace/Debug Functions
    (import "env" "trace" (func $trace (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "trace_num" (func $trace_num (param i32 i32 i64) (result i32)))
    (import "env" "trace_account" (func $trace_account (param i32 i32 i32 i32) (result i32)))
    (import "env" "trace_opaque_float" (func $trace_opaque_float (param i32 i32 i32 i32) (result i32)))
    (import "env" "trace_amount" (func $trace_amount (param i32 i32 i32 i32) (result i32)))

    ;; Float Arithmetic Functions
    (import "env" "float_from_int" (func $float_from_int (param i64 i32 i32 i32) (result i32)))
    (import "env" "float_from_uint" (func $float_from_uint (param i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_set" (func $float_set (param i32 i64 i32 i32 i32) (result i32)))
    (import "env" "float_compare" (func $float_compare (param i32 i32 i32 i32) (result i32)))
    (import "env" "float_add" (func $float_add (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_subtract" (func $float_subtract (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_multiply" (func $float_multiply (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_divide" (func $float_divide (param i32 i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_root" (func $float_root (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_pow" (func $float_pow (param i32 i32 i32 i32 i32 i32) (result i32)))
    (import "env" "float_log" (func $float_log (param i32 i32 i32 i32 i32) (result i32)))
)


"#).unwrap())
        ,
        gc_enabled: false,
        threads_enabled: false,
        min_funcs: 1,
        simd_enabled: false,
        tail_call_enabled: false,
        generate_custom_sections: false,
        allowed_instructions: InstructionKinds::new(&[InstructionKind::NumericInt, InstructionKind::Reference, InstructionKind::Variable, InstructionKind::MemoryInt, InstructionKind::Control]),
        multi_value_enabled: false,
        sign_extension_ops_enabled: false,
        saturating_float_to_int_enabled: false,
        reference_types_enabled: false,
        extended_const_enabled: false,
        bulk_memory_enabled: false,
        memory64_enabled: false,
        export_everything: true,
        ..Default::default()
    };
    let data = std::slice::from_raw_parts(ptr, len);
    let mut u = Unstructured::new(data);
    let Ok(module) = Module::new(config, &mut u) else {
        return Slice {
            ptr: std::ptr::null(),
            len: 0,
        };
    };

    let data = module.to_bytes();
    if data.len() == 0 {
        return Slice {
            ptr: std::ptr::null(),
            len: 0,
        };
    }
    let ret_ptr = data.as_ptr() as *const u8;
    let ret_len = data.len();
    std::mem::forget(data);
    Slice {
        ptr: ret_ptr,
        len: ret_len,
    }
}

#[unsafe(no_mangle)]
unsafe fn fuzz_free_module(slice: Slice) {
    if !slice.ptr.is_null() {
        let _ = Vec::from_raw_parts(slice.ptr as *mut u8, slice.len, slice.len);
    }
}
