# Plan-based Delta scan — remaining work

**Goal.** Move the delta-kernel-rs ↔ duckdb-delta boundary from *SQL strings* to a
*protobuf plan IR*. The kernel owns the SM/Plan SDK (C ABI + generated proto structs +
RAII C++ adapters, all kernel-shipped). DuckDB consumes them **transitively** and does
the driving + IR→DuckDB `LogicalOperator` translation. End state: `plan_to_sql.rs`
(960 LOC of DuckDB dialect embedded in the kernel) and the `_sql` FFI are deleted; the
kernel is dialect-pure.

This file is the durable, detailed backlog. Task IDs (`#NN`) match the session task list.

---

## Where the boundary is today (the seam to move)

- **Kernel side** (`~/delta-kernel-rs`, branch `prototype/duckdb-plan-based-scan`):
  - `StateMachine` trait: `get_step() -> EngineRequest`, `submit(EngineResponse) -> NextStep<R>`.
    Requests: `Reduce{nodes, terminal, sink}`, `SchemaQuery{file_path}`. Responses:
    `Reducer`, `Schema`, `Empty`. Scan SM terminal `R` = `ResultPlan` (SSA IR DAG).
  - IR `NodeKind`: ListFiles, ScanParquet, ScanJson, Values, Project, Filter, Load,
    MaxByVersion, EquiJoin, UnionAll + Expression/Predicate/Scalar tree.
  - `ffi/src/duckdb/plan_to_sql.rs` (**960 LOC**) lowers IR→DuckDB SQL — the *leaky*
    module: kernel embeds engine dialect. **This is what the re-arch deletes.**
  - `ffi/src/duckdb/proto_convert.rs`: `result_plan_to_proto` (all 10 NodeKinds) — the
    alternate transport, not yet the live path.
  - `ffi/proto/{plan,expressions,schema}.proto`: the wire schema.
  - `ffi/build.rs`: prost (Rust structs) **+ now `protoc --cpp_out`** (C++ structs) —
    Stage 0, this session.
  - FFI exports (`ffi/src/duckdb/sm.rs`): `kdf_scan_open`, `kdf_sm_get_step`,
    `kdf_sm_reduce_sql`, `kdf_sm_submit_reduce`, `kdf_sm_result_sql`, `kdf_sm_free`,
    **+ now `kdf_sm_result_plan` / `kdf_bytes_free`** (Stage 0).

- **Engine side** (`~/duckdb/duckdb-delta`):
  - `src/include/functions/delta_scan/sm_sdk.hpp` — `DriveScan()`: the driver loop.
    DuckDB pulls steps, runs Reduce SQL via `RunSqlToArrow`, submits Arrow back, and on
    DONE gets the final SQL from `kdf_sm_result_sql`. **This loop is the cutover surface.**
  - Consumes the kernel through **strings+ints only** — never sees `NodeKind`.

**The two call sites the IR path must replace in `DriveScan`:**
1. `kdf_sm_reduce_sql` → `RunSqlToArrow` (the per-Reduce SQL)  → **Stage 3**
2. `kdf_sm_result_sql` → `return sql`  (the terminal plan SQL) → **Stage 2 + Stage 4**

---

## Stage 0 — kernel emits C++ proto structs + round-trip baseline  (#56, IN PROGRESS)

**Done (verified):**
- `build.rs` runs `protoc --cpp_out` on the same 3 schemas → `target/ffi-headers/proto-cpp/{plan,expressions,schema}.pb.{h,cc}`. Honors `$PROTOC`.
- Version match proven: generated `.pb.h` stamps `PROTOBUF_VERSION 6033004` == vcpkg libprotobuf 33.4.
- `kdf_sm_result_plan` + `kdf_bytes_free` FFI exports added (SM-handle form).
- Committed + pushed to `origin/prototype/duckdb-plan-based-scan` (`7660bb279`).
- CMake wiring in `duckdb-delta/CMakeLists.txt`:
  - `find_package(Protobuf CONFIG REQUIRED)`, inject `PROTOC=<vcpkg protoc>` into the kernel ExternalProject's env so kernel codegen uses vcpkg protoc.
  - `vcpkg.json` gained `"protobuf"`.
  - `.pb.cc` produced by the ExternalProject; `add_custom_command(OUTPUT … DEPENDS delta_kernel)` gives Make a rule (BUILD_BYPRODUCTS is advisory under Unix Makefiles / CMake 3.16).
  - `delta_proto` OBJECT lib compiles the `.pb.cc` at **C++17 in isolation** (`CXX_STANDARD 17` +
    its own libprotobuf link). It is **NOT** folded into the shipping extension.
  - **Round-trip is a standalone `proto_roundtrip` executable** (`test/proto_roundtrip.cpp`), C++17,
    links `delta_proto` + libprotobuf. Serialize/parse a `ResultPlan`, assert structure survives.
  - **The shipping extension stays C++11 and does NOT link protobuf.** This is deliberate:
    protobuf 33 / Abseil advertise a *required* `cxx_std_17` compile-feature, which is a hard floor
    CMake will NOT lower — not via `CMAKE_CXX_STANDARD`, not via an explicit target `CXX_STANDARD 11`,
    not via `$<LINK_ONLY:>` (CMake 3.16 doesn't honor LINK_ONLY for INTERFACE_COMPILE_FEATURES), not
    by clearing the property on `protobuf::libprotobuf` (Abseil re-adds it transitively). Any
    extension target with protobuf anywhere in its link closure is forced to c++17, which breaks ODR
    against the C++11-built libduckdb (weak c++17 `LogicalType::VARCHAR` inline vars vs libduckdb's
    strong out-of-line defs → "multiple definition of VARCHAR"). **Verified empirically** in a clean
    build: parquet=c++11, core=c++11, delta (with protobuf)=c++17. So folding protobuf INTO the
    extension is a **Stage 2** concern (below), not Stage 0.

**Stage 0 — DONE + validated.**
- [x] `make release` green, extension at c++11, `proto_roundtrip` runs `OK` (version match).
- [x] Acceptance harness **1643/1656 — exactly the baseline** (0 regressions; the 13 non-passing are
      the same pre-existing known-fails: `is_add` struct-key, INTEGER→DATE cast, empty-stats-JSON,
      unknown-feature). Run on the httpfs-free build (see OpenSSL note) with the DV-001=1993 spot check.
- [x] Committed: duckdb-delta `74dbda6` (proto-clean branch), kernel `7660bb279`.

**Stage 1 — DONE (kernel side) + validated.**
- [x] `kdf_sm_reduce_plan` + `pending_reduce_proto` added; `cargo check` green; committed kernel
      `84e0183fb`. Additive — SQL path still drives, so the 1643/1656 parity above covers it.
- [ ] (Deferred to Stage 2/3) the ENGINE side actually consuming `kdf_sm_reduce_plan` — that's the
      DeltaPlanBuilder work; nothing calls the new export yet.
- [ ] **C++ decode smoke-test**: drive a scan SM → `kdf_sm_result_plan` bytes → parse with the generated C++ `ResultPlan` struct → assert node count/root kind matches. Put it in the `acceptance_harness` or a tiny standalone. This closes the round-trip that the deleted `duckdb_proto_plan.rs` used to cover (a pure-Rust test can't drive a reduce-bearing SM now that DuckDB is the executor).
- [ ] Re-run the acceptance harness to confirm **no regression** (baseline 1643/1656) — proto wiring is additive, so it must stay green.
- [ ] Commit the CMake/vcpkg wiring on the duckdb-delta side.

**Gotchas captured:**
- The extension ExternalProject **clones the kernel from GitHub**, not the local tree. Any kernel change must be committed+pushed to `prototype/duckdb-plan-based-scan` AND the build's checkout refreshed (`git fetch && git checkout`, remove `-stamp/{build,done}`) or it silently builds stale code. (This bit us this session.)
- Pushes go to the OSS account via `github-oss.com` SSH remote, never the EMU `gh` account.

---

## Stage 1 — kernel FFI emits IR proto bytes for Reduce too  (#57)

Additive; both `_sql` and `_proto` exist in parallel.
- [ ] Add `kdf_sm_reduce_plan(sm) -> proto bytes` (the Reduce subplan analogue of
      `kdf_sm_reduce_sql`). Reuses `proto_convert.rs`; serialize the Reduce request's
      `{nodes, terminal, sink}` subplan.
- [ ] `result_plan` proto already emitted (`kdf_sm_result_plan`, Stage 0).
- [ ] Rust unit coverage for `proto_convert` on Reduce subplans (all NodeKinds that can
      appear in a Reduce: MaxByVersion, Filter, Project, Load, Values, ScanParquet/Json).

---

## Stage 1.5 — KERNEL-shipped RAII C++ SDK over the C ABI  (#58) — DONE + validated

Kernel ships `ffi/include/delta_kernel_sdk.hpp` (header-only, exceptions-based); `build.rs`
copies it into `target/ffi-headers/` next to the generated bindings so the engine gets it
transitively. Committed kernel `06dff7fce`, engine `sm_sdk.hpp`/`CMakeLists.txt`.
- [x] `KernelString` — owns a `kdf_string_free` char* (the `_sql` transport).
- [x] `KernelBytes` — owns a `kdf_bytes_free` buffer (the `_plan` proto transport; feed
      `data()`/`size()` to `ParseFromArray`).
- [x] `ScanStateMachine` — owns `KdfSM*` (freed on scope exit incl. on throw), typed
      `Open`/`GetStep`/`ReduceSql`/`ReducePlan`/`SubmitReduce`/`ResultSql`/`ResultPlan`, `Step`
      enum. Replaced the raw pointer + try/catch double-free loop in `sm_sdk.hpp::DriveScan`.
- [x] `KernelException` — every call turns `out_err` into a thrown error.
- [x] FFI-header include is overridable via `DELTA_KERNEL_SDK_FFI_HEADER` (engine points it at
      its patched `generated_delta_kernel_ffi.hpp`); the ExternalProject copies the SDK header
      into `codegen/include` alongside the patched header.
- [x] VALIDATED: builds clean, DriveScan rewritten on the SDK, DV-001=1993, full corpus
      **1643/1656** (baseline, no regression).
- [x] `Snapshot` / `Scan` RAII adapters (added via a real ABI split, not a façade). The kernel C ABI
      was split from one fused `KdfSM` into two handles mirroring the kernel's domain model:
      `KdfSnapshot` (open → drive → `kdf_snapshot_version` → `kdf_snapshot_scan` builds a scan; holds
      the snapshot as `Arc` so it can build more than one) and `KdfScan` (drive → `ResultPlan`). The
      ~230-line reduce/step/submit machinery is a generic `ReduceDriver<R>` shared by both. SDK:
      `Snapshot` (`Open`/`GetStep`/`Reduce*`/`SubmitReduce`/`Version`/`Scan()`) + `Scan`
      (`GetStep`/`Reduce*`/`SubmitReduce`/`Result*`). `DriveScan` = open Snapshot → drive → `.Scan()`
      → drive → `ResultSql`. VALIDATED: DV-001=1993, corpus **1643/1656** (baseline). Kernel commits
      `a395425ed` + `90c3666c7` (`R: 'static` fix); engine `sm_sdk.hpp`.
- Deferred (add when a consumer needs it): a C++ `EngineRequest` variant (Reduce vs SchemaQuery vs
  Done) — the SM resolves SchemaQuery internally so the engine only sees Reduce/Done (`Step` covers
  it); snapshot `schema()` accessor (kernel has `Snapshot::schema()`, no ABI export yet).

---

## Stage 2 — DeltaPlanBuilder: proto IR → DuckDB LogicalOperator  (#59)

The heart of the re-arch: the engine lowers the IR itself, in C++, to a DuckDB
`LogicalOperator` tree (not SQL).

**⚠ C++11/17 boundary (the constraint Stage 0 surfaced).** protobuf 33 forces c++17, DuckDB
extension TUs are c++11, and the two can't share a translation unit without ODR breakage. So
`DeltaPlanBuilder` must be split: a **c++17 "decode" TU** that owns all `#include "*.pb.h"` and
turns proto bytes into a *plain, non-protobuf* C++ IR (small structs / a visitor interface with no
protobuf types in its header), linked in as an isolated object (like `delta_proto` today); and the
**c++11 "build" side** (the rest of the extension) that consumes only that protobuf-free surface to
emit `LogicalOperator`. The proto types must never appear in a header included by a c++11 TU.
Design this seam first — it's the load-bearing wall of the whole re-arch.

- [ ] **Spike first**: translate just `ScanParquet` + `Filter` → `LogicalGet` +
      `LogicalFilter`, run one workload, prove the shape works end-to-end.
- [ ] Then cover the full NodeKind set: ListFiles, ScanParquet, ScanJson, Values,
      Project, Filter, Load (→ the existing `delta_load` TVF or a `LogicalTableFunction`),
      MaxByVersion (→ aggregate/window), EquiJoin (→ `LogicalComparisonJoin`),
      UnionAll (→ `LogicalSetOperation`).
- [ ] Expression/Predicate/Scalar tree → DuckDB `Expression` (Column, Literal incl
      Decimal/Timestamp/Date, Binary, Unary, Variadic, If, Struct, ParseJson,
      MapToStruct, Transform, Opaque/Unknown).
- [ ] **Fallback**: for any node not yet handled, emit an unbound `SelectStatement`
      (reuse `plan_to_sql` output for that subtree) so migration is incremental and never
      regresses parity. Delete the fallback per-node as coverage lands.
- [ ] This is where the ~960 LOC of lowering *moves to* (C++, engine-owned).

---

## Stage 3 — drive Reduce via IR too (one translation path)  (#60)

- [ ] Replace `kdf_sm_reduce_sql`→`RunSqlToArrow` in `DriveScan` with
      `kdf_sm_reduce_plan`→`DeltaPlanBuilder`→execute the LogicalOperator→Arrow.
- [ ] Now Reduce and terminal use the **same** IR→LogicalOperator translator (Stage 2).
- [ ] Keep the async BLOCKED/wake driving semantics intact (the driver loop structure
      doesn't change, only what each step returns).

---

## Stage 4 — cutover + delete the SQL path  (#61, #44)

- [ ] Flip `DriveScan` to use only the `_plan` (IR) FFI.
- [ ] Delete `plan_to_sql.rs` (960 LOC) from the kernel.
- [ ] Delete `kdf_sm_reduce_sql` / `kdf_sm_result_sql` FFI exports.
- [ ] Delete the SQL fallback in `DeltaPlanBuilder` (Stage 2) once all nodes are covered.
- [ ] Kernel is now DuckDB-dialect-pure; the only engine-specific thing it ships is the
      proto schema + C ABI + RAII headers (all generic transport, no SQL).
- [ ] Full acceptance-workloads parity run at each removal to prove no regression.

---

## Non-blocking / parallel cleanups (not on the critical path)

- **#53** Surface the metadata reconciliation as visible child-plan nodes under the
  `DYNAMIC_SCAN` operator in EXPLAIN. (Likely already largely landed via the
  `DeltaScanAttachReconciliationSubplan` OptimizerExtension — verify + finish.)
- **#54** Simplify SM + driving abstractions, run the full review gamut
  (architecture-reviewer, scovich-reviewer, delta-protocol-reviewer, code-review-refactor,
  ai-slop-reviewer). Do this *after* Stage 2 lands, since that's the big shape change.
- **#24** DV resolution off the data critical path (M3) — pre-existing, independent.
- Update `PROTOTYPE.md` + the demo script for the no-env-var world (single default path).
- PR #322 description edit still needs the OSS account or a manual paste
  (`/tmp/pr322_new_body.md`) — the EMU `gh` account 403s on `duckdb/duckdb-delta`.

## Environmental gotchas on this devbox (not code issues)

- **httpfs vs system libcurl.** A from-scratch build re-fetches `duckdb-httpfs` (pinned SHA in
  `extension_config.cmake`) into `build/release/_deps/httpfs_extension_fc-src`. Its
  `src/httpfs_curl_client.cpp:99` uses `CURLSSLOPT_AUTO_CLIENT_CERT | CURLSSLOPT_NATIVE_CA`, both
  added in libcurl 7.77.0; the devbox system libcurl is 7.68.0 → "not declared in this scope". Guard
  those two with `#if defined(...)` (a local build-tree patch, lost on re-fetch) or provide a newer
  curl via vcpkg. This blocks the FULL `make release` (delta itself builds fine); the delta
  extension + acceptance_harness only need libduckdb, which pulls httpfs transitively.
- **Dual-OpenSSL abort at startup (the acceptance_harness / duckdb-shell crash).** In a fully clean
  build the `duckdb` shell aborts (SIGABRT) even on `--version`, and the acceptance_harness cores —
  BOTH before `main`. Core-dump backtrace: `abort` ← `libssl.so.1.1 + 0x222d6` ← `call_init`
  (ld-linux) ← `_dl_start_user`. Root cause: TWO OpenSSL 1.1 runtimes in one process. The kernel's
  Rust lib links OpenSSL **statically** (`OPENSSL_STATIC=1` in CMakeLists → 264 static
  `OPENSSL_init`/`SSL_*`/`CRYPTO_*` symbols in the binary), while `libduckdb.so` → system
  `libcurl.so.4` → `libgssapi_krb5`/`libkrb5` → **dynamic** `libssl.so.1.1` + `libcrypto.so.1.1`.
  The dynamic OpenSSL's library constructor aborts when it finds the static one already initialized.
  The working older builds (`build/relfast`, 06-27) have **no dynamic libssl** — that's the tell.
  This is 100% a BUILD-ENVIRONMENT LINKAGE CONFLICT, independent of the proto/Stage-0 work (which
  touches no TLS code; `proto_roundtrip`, linking neither libduckdb nor curl, runs green). It was
  latent and got exposed when this clean build linked the SYSTEM libcurl (dragging Kerberos → dynamic
  OpenSSL); the httpfs/curl guard above is the same story (httpfs now compiles vs system curl).
  Precise symbol picture (VERIFIED): the UNPREFIXED `OPENSSL_init_ssl`/`OPENSSL_init_crypto`/
  `SSL_new` `T` symbols in the binary come from the vcpkg **static** `libssl.a`/`libcrypto.a`, which
  **httpfs** links (`httpfs .../CMakeLists.txt: find_package(OpenSSL REQUIRED)`). `libduckdb.so` also
  has a NEEDED on the SYSTEM `libcurl.so.4`, which drags `libgssapi_krb5`→`libkrb5`→**dynamic**
  `libssl.so.1.1`. Two OpenSSLs in one process → the dynamic one's `call_init` ctor aborts.
  IMPORTANT: the kernel does NOT contribute here — `openssl-sys` is not in its dep tree, and aws-lc-rs
  exports PREFIXED `aws_lc_*` symbols. So dropping the kernel's `OPENSSL_STATIC=1` (done in the Stage-0
  commit) is correct hygiene but does NOT fix this abort — VERIFIED: shell still cores after that
  change. The collision is entirely **httpfs (static vcpkg OpenSSL) vs system libcurl (dynamic
  OpenSSL)**. Fix must target httpfs's OpenSSL or remove httpfs (option d) — NOT the kernel.
  FIX OPTIONS (separate task): (a) build httpfs against a vcpkg curl that uses the SAME OpenSSL the
  kernel statically links (or a curl without gssapi/kerberos); (b) make the kernel link OpenSSL
  dynamically (drop `OPENSSL_STATIC=1`) so there's ONE OpenSSL; (c) use rustls end-to-end and a
  curl built without OpenSSL; (d) for LOCAL-workload parity only, build with NO httpfs.
  ⚠ GOTCHA when doing (d): `make release EXT_CONFIG=<no-httpfs>.cmake` is IGNORED once
  `build/release/CMakeCache.txt` already baked `DUCKDB_EXTENSION_CONFIGS` — `make release` does NOT
  re-pass it. You must build the no-httpfs config in a FRESH `build/<name>` dir (or hand-edit the
  cached `DUCKDB_EXTENSION_CONFIGS` and force a full reconfigure). Confirmed this session: the
  override was silently dropped and httpfs was reused from cache.
  Until fixed, the acceptance harness can't run in this tree on this devbox — but this predates and
  is orthogonal to the proto IR work (proto_roundtrip, which links neither libduckdb nor curl, is green).

---

## Design rationale (why this cut)

Per the software-design-advisor pass: the SQL-string boundary makes `plan_to_sql` a
**deep module in the wrong crate** — the kernel embeds DuckDB dialect (information
leakage: the same "how DuckDB spells this" decision lives in the kernel and must be
reimplemented by every other engine). Moving lowering to the engine makes the kernel's
interface the *IR itself* (a genuinely general, engine-agnostic abstraction) and pushes
dialect specialization **upward into the engine** where it belongs. Each engine writes
its own ~1-visitor lowering; the kernel stays pure. Proto is the transport because the
scaffolding already exists (`proto_convert.rs`, the `.proto` schema) and it gives a
single source of truth for the wire types on both sides (no encoder/decoder skew).
