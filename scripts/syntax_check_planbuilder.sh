#!/bin/sh
# Fast syntax-check for the Phase-D DeltaPlanBuilder island TUs against the real
# DuckDB + fresh kernel FFI + generated proto headers. Avoids the ~30min full
# extension build during D1..D7 iteration (the single linking build lands at D8).
#
set -e
DD=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DUCKDB_INC=$DD/duckdb/src/include
DELTA_INC=$DD/src/include
KERNEL_INC=$DD/vendor/delta-kernel-rs/ffi/include
# The PATCHED generated FFI header (generated_delta_kernel_ffi.hpp) + the refreshed delta_kernel.hpp
# live here; delta_utils.hpp hard-includes the patched name, so put this dir first. Some island TUs
# only need the fresh worktree header, but this dir is a superset for syntax-checking.
PATCHED_FFI_DIR=$DD/build/release/codegen/include
PROTO_INC=$DD/build/release/rust/local-target/proto-cpp
VCPKG_INC=$DD/build/release/vcpkg_installed/x64-linux/include

for tu in "$@"; do
  g++ -std=c++17 -fsyntax-only -w \
    -DDEFINE_DEFAULT_ENGINE -DDEFINE_DEFAULT_ENGINE_BASE \
    -I"$PATCHED_FFI_DIR" -I"$DELTA_INC" -I"$DUCKDB_INC" -I"$KERNEL_INC" -I"$PROTO_INC" -I"$VCPKG_INC" \
    "$tu" && echo "OK: $tu"
done
