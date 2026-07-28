//! DuckDB logical-plan/state-machine SDK support.
//!
//! The engine drives snapshot and scan state machines, executes protobuf `ResultPlan`s, and hands
//! Arrow reducer results back to the kernel. No engine SQL is generated or transported here.
//!
//! [`Scan::scan_metadata_state_machine`]: delta_kernel::scan::Scan::scan_metadata_state_machine

use std::ffi::{c_char, CString};
use std::ptr;
use std::sync::Arc;

use delta_kernel::engine::default::DefaultEngineBuilder;
use delta_kernel::object_store::local::LocalFileSystem;
use delta_kernel::Engine;
use url::Url;

pub mod proto;
pub mod proto_convert;
pub mod sm;

/// Build a local-filesystem-backed kernel default engine.
pub(crate) fn build_local_engine() -> Arc<dyn Engine> {
    Arc::new(DefaultEngineBuilder::new(Arc::new(LocalFileSystem::new())).build())
}

/// Build a kernel default engine whose object store is chosen by the table URL's scheme — local
/// for `file://`, S3 for `s3://`/`s3a://` (creds read from the standard `AWS_*` env vars and passed
/// as object_store options). Used by the SM driver so it can list the log / read footers on cloud
/// tables; DuckDB (httpfs) handles the data + reduce-query I/O separately.
pub(crate) fn build_engine_for_url(url: &Url) -> Result<Arc<dyn Engine>, String> {
    use delta_kernel::engine::default::storage::store_from_url_opts;
    let mut opts: Vec<(String, String)> = Vec::new();
    if matches!(url.scheme(), "s3" | "s3a") {
        if let Ok(v) = std::env::var("AWS_REGION").or_else(|_| std::env::var("AWS_DEFAULT_REGION"))
        {
            opts.push(("region".into(), v));
        }
        if let Ok(v) = std::env::var("AWS_ACCESS_KEY_ID") {
            opts.push(("access_key_id".into(), v));
        }
        if let Ok(v) = std::env::var("AWS_SECRET_ACCESS_KEY") {
            opts.push(("secret_access_key".into(), v));
        }
        if let Ok(v) = std::env::var("AWS_SESSION_TOKEN") {
            opts.push(("session_token".into(), v));
        }
    }
    let store =
        store_from_url_opts(url, opts).map_err(|e| format!("build object store for {url}: {e}"))?;
    Ok(Arc::new(DefaultEngineBuilder::new(store).build()))
}

/// Resolve a user-supplied table location to a `Url`. Accepts an existing URL (e.g. `file://`,
/// `s3://`) or a local filesystem path, which is canonicalized into a `file://` directory URL.
pub(crate) fn table_url(path: &str) -> Result<Url, String> {
    if let Ok(mut url) = Url::parse(path) {
        // Treat single-character "schemes" as Windows drive letters, not URL schemes.
        if url.scheme().len() > 1 {
            // The table root is a directory: ensure a trailing slash so the kernel's
            // `Url::join("_delta_log/")` appends rather than replacing the last path segment
            // (without it, `s3://b/a/tbl` + `_delta_log/` resolves to `s3://b/a/_delta_log/`).
            if !url.path().ends_with('/') {
                let with_slash = format!("{}/", url.path());
                url.set_path(&with_slash);
            }
            return Ok(url);
        }
    }
    let abs = std::path::Path::new(path)
        .canonicalize()
        .map_err(|e| format!("canonicalize table path {path}: {e}"))?;
    Url::from_directory_path(&abs)
        .map_err(|()| format!("cannot build a file:// url from {}", abs.display()))
}

/// Write `msg` as a freshly-allocated C string into `*out_err` (when `out_err` is non-null).
/// The caller frees it with the matching exported string-free function.
///
/// # Safety
/// `out_err`, if non-null, must point to a writable `*mut c_char`.
pub(crate) unsafe fn write_err(out_err: *mut *mut c_char, msg: &str) {
    if out_err.is_null() {
        return;
    }
    let c = CString::new(msg).unwrap_or_default();
    unsafe { *out_err = c.into_raw() };
}

// ============================================================================
// Deletion-vector resolution for the plan-based data read
// ============================================================================

unsafe fn cstr_to_str<'a>(ptr: *const c_char, len: usize) -> Result<&'a str, String> {
    if ptr.is_null() {
        return Err("null string pointer".to_string());
    }
    let bytes = unsafe { std::slice::from_raw_parts(ptr as *const u8, len) };
    std::str::from_utf8(bytes).map_err(|e| format!("invalid UTF-8: {e}"))
}

fn resolve_dv_impl(
    root: &str,
    storage_type: &str,
    path_or_inline: &str,
    has_offset: bool,
    offset: i32,
    size_in_bytes: i32,
    cardinality: i64,
) -> Result<Vec<bool>, String> {
    use delta_kernel::actions::deletion_vector::{
        DeletionVectorDescriptor, DeletionVectorStorageType,
    };
    let engine = build_local_engine();
    let table_root = table_url(root)?;
    let storage_type: DeletionVectorStorageType = storage_type
        .parse()
        .map_err(|_| format!("unrecognized DV storageType: {storage_type:?}"))?;
    let descriptor = DeletionVectorDescriptor {
        storage_type,
        path_or_inline_dv: path_or_inline.to_string(),
        offset: if has_offset { Some(offset) } else { None },
        size_in_bytes,
        cardinality,
    };
    delta_kernel::scan::selection_vector(engine.as_ref(), &descriptor, &table_root)
        .map_err(|e| format!("resolve deletion vector: {e}"))
}

/// Resolve a Delta deletion-vector descriptor into a row-selection bool slice (true = keep) that
/// the C++ `DeltaDeleteFilter` applies during the parquet read. Returns an empty slice on error
/// (writes `*out_err`).
///
/// # Safety
/// All `*_ptr`/`*_len` pairs must describe valid UTF-8 byte ranges. `out_err`, if non-null, must
/// be a writable `*mut *mut c_char`. The returned slice's `ptr` (if non-null) must be freed once
/// with `free_bool_slice`.
#[no_mangle]
pub unsafe extern "C" fn delta_resolve_dv(
    table_root_ptr: *const c_char,
    table_root_len: usize,
    storage_type_ptr: *const c_char,
    storage_type_len: usize,
    path_or_inline_ptr: *const c_char,
    path_or_inline_len: usize,
    has_offset: bool,
    offset: i32,
    size_in_bytes: i32,
    cardinality: i64,
    out_err: *mut *mut c_char,
) -> crate::KernelBoolSlice {
    if !out_err.is_null() {
        unsafe { *out_err = ptr::null_mut() };
    }
    let outcome = std::panic::catch_unwind(|| {
        let root = unsafe { cstr_to_str(table_root_ptr, table_root_len) }?;
        let storage_type = unsafe { cstr_to_str(storage_type_ptr, storage_type_len) }?;
        let path_or_inline = unsafe { cstr_to_str(path_or_inline_ptr, path_or_inline_len) }?;
        resolve_dv_impl(
            root,
            storage_type,
            path_or_inline,
            has_offset,
            offset,
            size_in_bytes,
            cardinality,
        )
    });
    match outcome {
        Ok(Ok(bools)) => crate::KernelBoolSlice::from(bools),
        Ok(Err(msg)) => {
            unsafe { write_err(out_err, &msg) };
            crate::KernelBoolSlice::empty()
        }
        Err(_) => {
            unsafe { write_err(out_err, "delta_resolve_dv: panic during DV resolution") };
            crate::KernelBoolSlice::empty()
        }
    }
}
