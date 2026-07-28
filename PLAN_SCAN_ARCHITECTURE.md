# Delta plan scan architecture

`delta_scan` uses Delta Kernel's declarative scan state machines as its only metadata-planning
path. There is no SQL-plan transport or legacy state-machine fallback.

## Boundary

Delta Kernel ships a compiled C++17 SDK in `ffi/include/delta_kernel.hpp` and
`ffi/include/delta_kernel.cpp`. C++ consumers use move-only `Snapshot`, `Scan`, `ArrowBatch`, and
state-machine values. Raw C ABI handles and generated cbindgen declarations remain private to the
implementation file.

The SDK drives snapshot and scan state machines through an engine-provided `ResultPlan -> Arrow`
callback. A completed `Scan` owns its parsed protobuf `ResultPlan`.

## DuckDB execution

`DeltaPlanBuilder` lowers every kernel IR node directly to DuckDB parser and logical-plan objects.
Reduce plans and terminal plans use the same lowering path.

Public `delta_scan` binds a metadata-reconciliation child plan and a `PhysicalDeltaLoad` source.
The child materializes surviving file descriptors before the source starts. Each descriptor carries
its path, deletion vector, and all `metadata_derived` values, including commit `version` and
`fileConstantValues`. The native DuckDB multi-file reader then performs the parallel parquet scan,
partition constant injection, field-ID mapping, and deletion-vector filtering.

The optimizer replaces the bind-time reconciliation child after translating pushed DuckDB filters
to SDK-owned predicates. This places partition and statistics data-skipping expressions in the
kernel plan while retaining DuckDB's row-level filter for correctness.

## Invariants

- Reconciliation is build-before-probe; the multi-file reader never observes a growing file list.
- Every `LoadNode.metadata_derived` column is broadcast from its descriptor row.
- Declarative scan-file rows carry the full partition schema, even without a predicate.
- Unsupported predicate shapes disable kernel pushdown conservatively.
- Virtual columns such as `filename` and `file_row_number` bind through the custom scan operator
  without appearing in `SELECT *`.

## Verification

The focused gate is:

```sh
cargo test -p delta_kernel_ffi --lib \
  --features "default-engine-rustls,arrow,test-ffi,delta-kernel-unity-catalog,tracing,duckdb" \
  duckdb::

cmake --build build/nohttpfs \
  --target shell unittest delta_plan_builder_test proto_roundtrip -j2

DELTA_KERNEL_TESTS_PATH=vendor/delta-kernel-rs/kernel/tests/data \
  build/nohttpfs/test/unittest '[delta_kernel_rs]'
```
