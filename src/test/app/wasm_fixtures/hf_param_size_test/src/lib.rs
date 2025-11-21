#![cfg_attr(target_arch = "wasm32", no_std)]
#[cfg(not(target_arch = "wasm32"))]
extern crate std;

use xrpl_std::core::ledger_objects::current_escrow::{get_current_escrow, CurrentEscrow};
use xrpl_std::core::ledger_objects::traits::CurrentEscrowFields;
use xrpl_std::host::{compute_sha512_half, update_data};

#[unsafe(no_mangle)]
pub extern "C" fn finish() -> i32 {
    {
        // store data up to the limit of 4096 bytes in the current escrow object
        let test_data = [1u8; 4096usize];
        let res = unsafe {
            update_data(
                test_data.as_ptr(),
                test_data.len(),
            )
        };
        if res < 0 {
            return -10000
        }

        // read the data back from the current escrow object and compare it with the original data
        let current_escrow: CurrentEscrow = get_current_escrow();
        match current_escrow.get_data() {
            xrpl_std::host::Result::Ok(read_data) => {
                if read_data.len == test_data.len() {
                    for i in 0..read_data.len {
                        if read_data.data[i] != test_data[i] {
                            return -10001
                        }
                    }
                } else {
                    return -10002
                }
            }
            xrpl_std::host::Result::Err(_) => {
                return -10003
            },
        }
    }

    {
        // store data larger than 4096 bytes in the current escrow object, which should fail
        let test_data = [1u8; 4096usize + 1usize];
        let res = unsafe {
            update_data(
                test_data.as_ptr(),
                test_data.len(),
            )
        };
        if res != -12 { //result should be error: -12, data too large
            return -10004
        }
    }

    {
        // compute SHA512, which has a limit of 1024 bytes
        let test_data = [1u8; 1024usize];
        let mut hash_output = [0u8; 32];
        let res = unsafe {
            compute_sha512_half(
                test_data.as_ptr(),
                test_data.len(),
                hash_output.as_mut_ptr(),
                hash_output.len(),
            )
        };
        if res != 32 {
            return -10005
        }
    }

    {
        // compute SHA512, which has a limit of 1024 bytes, but the data is larger than that
        let test_data = [1u8; 1024usize+1usize];
        let mut hash_output = [0u8; 32];
        let res = unsafe {
            compute_sha512_half(
                test_data.as_ptr(),
                test_data.len(),
                hash_output.as_mut_ptr(),
                hash_output.len(),
            )
        };
        if res != -12 { //result should be error: -12, data too large
            return -10006
        }
    }

    1
}
