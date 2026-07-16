#!/bin/sh
# Fast syntax-check for the Phase-D DeltaPlanBuilder island TUs against the real
# DuckDB + fresh kernel FFI + generated proto headers. Avoids the ~30min full
# extension build during D1..D7 iteration (the single linking build lands at D8).
#
# Uses the FRESH kernel worktree's generated FFI header (target/ffi-headers), which
# already carries the Phase-B delta_* ABI, rather than the stale release-tree copy.
set -e
DD=/home/oussama.saoudi/duckdb/duckdb-delta
DUCKDB_INC=$DD/duckdb/src/include
DELTA_INC=$DD/src/include
KERNEL_INC=/home/oussama.saoudi/delta-kernel-rs/ffi/include
FRESH_FFI_DIR=/home/oussama.saoudi/delta-kernel-rs/target/ffi-headers
PROTO_INC=$DD/build/release/rust/src/delta_kernel/target/ffi-headers/proto-cpp
VCPKG_INC=$DD/build/release/vcpkg_installed/x64-linux/include

for tu in "$@"; do
  g++ -std=c++17 -fsyntax-only -w \
    -DDEFINE_DEFAULT_ENGINE -DDEFINE_DEFAULT_ENGINE_BASE \
    -I"$FRESH_FFI_DIR" -I"$DELTA_INC" -I"$DUCKDB_INC" -I"$KERNEL_INC" -I"$PROTO_INC" -I"$VCPKG_INC" \
    "$tu" && echo "OK: $tu"
done
