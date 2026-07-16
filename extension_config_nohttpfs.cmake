# httpfs-free extension config — for LOCAL-workload acceptance parity on this devbox.
#
# httpfs statically links vcpkg OpenSSL 3.x while its system libcurl dependency dynamically pulls
# system OpenSSL 1.1 (via gssapi/kerberos) — two OpenSSLs in one process abort in libssl's ctor before
# main (see REMAINING_WORK.md OpenSSL note). Local acceptance workloads are file:// tables that don't
# need httpfs, so dropping it yields a runnable duckdb for parity. Build in a FRESH build dir
# (build/nohttpfs) — `make release EXT_CONFIG=...` is ignored once a build dir cached
# DUCKDB_EXTENSION_CONFIGS.
duckdb_extension_load(delta
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Build the in-tree json extension from source (DONT_LINK: loaded at runtime, not statically linked)
# so the metadata reconciliation's delta_load(file_type:='json') exercises OUR patched json reader
# (JSONMultiFileInfo::FinalizeBindData initializes parser state on the custom MultiFileReader::Bind
# path). Without this the CLI autoloads the prebuilt installed json extension, which crashes.
duckdb_extension_load(json)
