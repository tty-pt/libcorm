#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

use libc::{c_char, c_void};
use std::ffi::{CStr, CString};
use std::ptr;

mod bindings;
pub use bindings::*;

pub const CM_MISS: u32 = u32::MAX;

/// Safe wrapper for a Corm handle.
pub struct Corm {
    hd: u32,
}

impl Corm {
    pub fn open(
        filename: Option<&str>,
        database: Option<&str>,
        ktype: u32,
        vtype: u32,
        mask: u32,
        flags: u32,
    ) -> Option<Self> {
        let c_filename = filename.map(|s| CString::new(s).unwrap());
        let c_database = database.map(|s| CString::new(s).unwrap());

        let hd = unsafe {
            corm_open(
                c_filename.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                c_database.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
                ktype,
                vtype,
                mask,
                flags,
            )
        };

        if hd == CM_MISS {
            None
        } else {
            Some(Corm { hd })
        }
    }

    pub fn handle(&self) -> u32 {
        self.hd
    }

    /// Creates a Corm handle from a raw handle ID.
    /// This should only be used for handles managed elsewhere (like in ndc C backend).
    pub unsafe fn from_handle(hd: u32) -> Self {
        Corm { hd }
    }

    /// Discover the value type of this map at runtime.
    pub fn get_vtype(&self) -> u32 {
        unsafe { corm_get_vtype(self.hd) }
    }

    /// Register a fixed-length type.
    pub fn reg_fixed(len: usize) -> u32 {
        unsafe { corm_reg(len) }
    }

    /// Register a variable-length type with a measure callback.
    pub fn reg_variable(measure: corm_measure_t) -> u32 {
        unsafe { corm_mreg(measure) }
    }

    /// Get the byte length of a stored value.
    pub fn len(type_id: u32, data: *const c_void) -> usize {
        unsafe { corm_len(type_id, data) }
    }

    /// Get a typed reference to a stored value.
    /// # Safety
    /// Caller must ensure `T` matches the actual value type of the map.
    pub unsafe fn get_raw<T>(&self, key: *const c_void) -> Option<&T> {
        let ptr = corm_get(self.hd, key);
        if ptr.is_null() { None } else { Some(&*(ptr as *const T)) }
    }

    /// Get a byte slice for a stored value using the map's value type.
    /// # Safety
    /// The returned slice borrows from the corm internal storage.
    pub unsafe fn get_bytes(&self, key: *const c_void) -> Option<&[u8]> {
        let ptr = corm_get(self.hd, key);
        if ptr.is_null() { return None; }
        let vtype = self.get_vtype();
        let len = Self::len(vtype, ptr);
        Some(std::slice::from_raw_parts(ptr as *const u8, len))
    }

    /// Store a typed value.
    /// # Safety
    /// Caller must ensure `T` matches the actual value type of the map.
    pub unsafe fn put_raw<T>(&self, key: *const c_void, value: &T) -> u32 {
        corm_put(self.hd, key, value as *const T as *const c_void)
    }

    /// Create an association (secondary index) from this map to a primary map.
    /// # Safety
    /// The callback must produce valid secondary keys. The link must be a valid
    /// primary map handle.
    pub unsafe fn assoc(&self, link: u32, cb: corm_assoc_t, userdata: *mut c_void) {
        corm_assoc(self.hd, link, cb, userdata)
    }

    /// Create a multi-key association (secondary index) from this map to a
    /// primary map. The callback produces multiple secondary keys from one
    /// primary entry.
    /// # Safety
    /// The callback must produce valid secondary key pointers. The link must
    /// be a valid primary map handle.
    pub unsafe fn assoc_multi(&self, link: u32, cb: corm_assoc_multi_t, userdata: *mut c_void) {
        corm_assoc_multi(self.hd, link, cb, userdata)
    }

    pub fn get(&self, key: *const c_void) -> *const c_void {
        unsafe { corm_get(self.hd, key) }
    }

    pub fn get_str(&self, key: &str) -> Option<&str> {
        let c_key = CString::new(key).unwrap();
        let val_ptr = unsafe { corm_get(self.hd, c_key.as_ptr() as *const c_void) };
        if val_ptr.is_null() {
            None
        } else {
            unsafe { CStr::from_ptr(val_ptr as *const c_char).to_str().ok() }
        }
    }

    pub fn put(&self, key: *const c_void, value: *const c_void) -> u32 {
        unsafe { corm_put(self.hd, key, value) }
    }

    pub fn put_str(&self, key: &str, value: &str) -> u32 {
        let c_key = CString::new(key).unwrap();
        let c_value = CString::new(value).unwrap();
        unsafe {
            corm_put(
                self.hd,
                c_key.as_ptr() as *const c_void,
                c_value.as_ptr() as *const c_void,
            )
        }
    }

    pub fn del(&self, key: *const c_void) {
        unsafe { corm_del(self.hd, key) }
    }

    pub fn drop(&self) {
        unsafe { corm_drop(self.hd) }
    }

    pub fn iter(&self, key: *const c_void, flags: u32) -> Cursor {
        let cur_id = unsafe { corm_iter(self.hd, key, flags) };
        println!("DEBUG: corm_iter(hd={}, key={:?}, flags={}) returned cur_id={}", self.hd, key, flags, cur_id);
        Cursor { cur_id }
    }

    pub fn get_multi(&self, key: *const c_void) -> u32 {
        unsafe { corm_get_multi(self.hd, key) }
    }

    pub fn next(&self, cur_id: u32) -> Option<(*const c_void, *const c_void)> {
        let mut key: *const c_void = ptr::null();
        let mut value: *const c_void = ptr::null();
        let res = unsafe { corm_next(&mut key, &mut value, cur_id) };
        if res == 1 {
            Some((key, value))
        } else {
            None
        }
    }

    pub fn fin(&self, cur_id: u32) {
        unsafe { corm_fin(cur_id) }
    }
}

impl Drop for Corm {
    fn drop(&mut self) {
        unsafe { corm_close(self.hd) }
    }
}

pub struct Cursor {
    cur_id: u32,
}

impl Cursor {
    pub fn next(&mut self) -> Option<(*const c_void, *const c_void)> {
        let mut key: *const c_void = ptr::null();
        let mut value: *const c_void = ptr::null();
        let res = unsafe { corm_next(&mut key, &mut value, self.cur_id) };
        println!("DEBUG: corm_next(cur_id={}) returned res={} (key={:?}, value={:?})", self.cur_id, res, key, value);
        if res == 1 {
            Some((key, value))
        } else {
            None
        }
    }
}

impl Drop for Cursor {
    fn drop(&mut self) {
        unsafe { corm_fin(self.cur_id) }
    }
}

pub fn save() {
    unsafe { corm_save() }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_corm_open_in_memory() {
        let q = Corm::open(None, None, corm_tbi_CM_STR, corm_tbi_CM_STR, 0xFF, 0);
        assert!(q.is_some());
        let q = q.unwrap();
        q.put_str("key1", "value1");
        assert_eq!(q.get_str("key1"), Some("value1"));
        assert_eq!(q.get_str("key2"), None);
    }

    #[test]
    fn test_fixed_type_and_raw_access() {
        // Register a fixed type for u32
        let t = Corm::reg_fixed(4);
        assert_ne!(t, CM_MISS);

        // Open a map with this type for values
        let q = Corm::open(None, None, corm_tbi_CM_STR, t, 0xFF, 0).unwrap();
        let key = std::ffi::CString::new("count").unwrap();
        let val: u32 = 42;

        unsafe {
            q.put_raw(key.as_ptr() as *const c_void, &val);
            let stored: &u32 = q.get_raw(key.as_ptr() as *const c_void).unwrap();
            assert_eq!(*stored, 42);
            assert_eq!(Corm::len(t, stored as *const u32 as *const c_void), 4);
        }
    }

    #[test]
    fn test_get_vtype() {
        let q = Corm::open(None, None, corm_tbi_CM_STR, corm_tbi_CM_STR, 0xFF, 0).unwrap();
        assert_eq!(q.get_vtype(), corm_tbi_CM_STR);
    }
}
