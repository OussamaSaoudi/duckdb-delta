# Implementation plan — kernel plan-IR SDK

How to get from today's state to the architecture in `SDK_VISION.md` / `SDK_API_DESIGN.md` /
`SDK_FFI_INVESTIGATION.md`, without ever breaking parity (**1643/1656**).

## Guiding invariants (hold at every step)

1. **The SQL path stays green until the very end.** Today's `sm_sdk.hpp` → `DriveScan` → `delta_scan`
   (`kdf_*`/`_sql` exports) keeps driving the engine through Phases A–E. Delete it only in Phase F,
   after the proto path is proven at parity.
2. **Every phase ends green** — `make release` builds, acceptance harness at baseline, committed +
   pushed (kernel → `prototype/duckdb-plan-based-scan`, engine → fork `proto-clean`).
3. **New before old.** Each new export/handle/type lands and is validated *additively*, alongside
   the working path, before anything is cut over.
4. **Kernel core is never touched.** All work is in `ffi/src/duckdb/` (Rust) and `duckdb-delta`
   (C++). If a change wants to touch `kernel/src/`, stop — the design says it shouldn't.

---

## Phase A — De-risk the one unknown (SPIKE, throwaway) — ✅ DONE: PROCEED with enum-handle ABI

**Outcome (verified):** a `#[cfg(test)]` spike in `ffi/src/duckdb/sm.rs` defined
`enum FfiSm { Snapshot(ReduceDriver<Snapshot>), Scan(ReduceDriver<ResultPlan>) }` + `unsafe impl Send`
+ `#[handle_descriptor(target = FfiSm, mutable = true, sized = true)] SmHandle`, and exercised the
full handle lifecycle (`Box::new(sm).into()` → `as_mut()` → 2-arm forward → `drop_handle()`). It
**compiled and the test passed** (`ffi_sm_handle_lifecycle_compiles ... ok`). The only friction was
`E0446` (make `FfiSm` `pub`, since `#[handle_descriptor]` emits a `pub` impl). **Decision: proceed
with the enum-handle ABI; no fallback to raw `*mut` needed.** Spike reverted (throwaway).
Note: `crate::handle::Handle` needs the `internal-api` feature to be importable in-crate — Phase B's
handles live in the same crate so they don't hit this, but tests referencing `Handle` directly must
enable `internal-api`. Also observed: pre-existing `tests/duckdb_kdf_scan.rs` + `tests/dump_sql.rs`
are already stale (reference removed `kdf_scan_*`); clean them up opportunistically in Phase B/F.

**Goal:** prove `#[handle_descriptor(mutable=true)]` can wrap the `!Send` state machine before
committing the whole ABI to it (FFI §6/§7). This is the single highest-risk item; everything else is
assembling existing pieces.

- A1. In a scratch module: `enum FfiSm { Snapshot(StateMachine<Snapshot>), Scan(StateMachine<ResultPlan>) }`
      (reuse today's per-`R` driver as the arm), `unsafe impl Send for FfiSm`, `#[handle_descriptor]`.
- A2. Confirm it **compiles** (the `Send` bound is the question) and that a `Handle<SmHandle>` can be
      created, `as_mut`'d through a real snapshot drive to `Done`, and `drop_handle`'d.
- A3. Decision gate:
      - **compiles + drives** → proceed to Phase B with the enum-handle ABI.
      - **doesn't** → fall back: keep `#[handle_descriptor]` for the *value* handles only
        (`Snapshot`/`Scan`, already `Send+Sync`), keep the SM handle as today's raw `*mut` (which
        already relies on the same single-owner invariant). Record the decision; the rest of the plan
        is unchanged except the SM handle's free/lifecycle.

**Exit:** we know the SM handle shape. ~½ day. Throwaway code.

---

## Phase B — Kernel FFI: the new proto-only ABI (additive) — ✅ DONE (kernel `20688963e`)

**Outcome:** 20 `delta_*` exports + 5 handle types (`DeltaSm` enum, `DeltaReducer`,
`DeltaFinishedReducer`, `DeltaSnapshotValue`, `DeltaScanValue`), self-contained
`delta_bytes_free`/`delta_string_free` (no `kdf_*` leakage into the new surface),
`proto_convert::schema_to_proto`. Compiles; structural test `delta_abi_tests` passes (open → next →
reduce_plan decodes as ResultPlan proto → take_reducer → arm-guard); cbindgen renders all exports +
opaque typedefs. **Legacy `kdf_*` path verified untouched** (diff removed 0 `kdf_` lines) → parity
net intact. Reused the existing `KdfSnapshot`/`KdfScan`/`ReduceDriver` internals unchanged (the
`DeltaSm` arms wrap them), so zero logic duplication.

**Goal:** the full `delta_*` export surface (FFI §3) exists and is exercised by a Rust round-trip,
*without touching* the live `kdf_*`/`_sql` path.

- B1. **Error plumbing — REFINED after reading the code.** The idiomatic `ExternResult<Handle<X>>`
      requires an `AllocateError` source, which mainline gets from a `Handle<SharedExternEngine>`
      parameter on every export. But our SDK's `open` builds the engine INTERNALLY and takes no engine
      handle (that's the whole premise — `OpenSnapshot(path, version)`). Threading an engine handle
      through purely for error allocation would contradict the API. **Decision:** keep the existing
      SDK file's proven pattern — opaque `*mut FfiSm` pointers (null-on-error) + `out_err: *mut *mut
      c_char` string, freed by `kdf_string_free`/`delta_string_free`; C++ turns `out_err` into a thrown
      `DeltaError`. This means we do NOT use the `Handle<H>` *wrapper type* (whose main value is the
      `ExternResult` integration we're declining) — but we get the same opaque-pointer + Box lifecycle
      via the file's existing `Box::into_raw`/`from_raw` + `init_out!`/`leak_*` helpers, which Phase A
      showed are equivalent in safety. The `#[handle_descriptor]` route stays available if a future
      consumer wants the engine handle. The engine→kernel direction (`EngineError`) is a
      `delta_sm_submit_error` export carrying an error kind + message string.
- B2. **Handles** (FFI §2): `FfiSm` (from Phase A), `SharedScan`, `ReducerH`, `FinishedReducerH`;
      reuse mainline `SharedSnapshot`. Free fns for each.
- B3. **SM exports**: `delta_open_snapshot`, `delta_sm_next`, `delta_sm_reduce_plan`,
      `delta_sm_take_reducer`, `delta_sm_submit`, `delta_sm_submit_error`, `delta_sm_build_snapshot`,
      `delta_sm_build_scan`, `delta_sm_free`. `next`/`submit` 2-arm-forward on `FfiSm`; `build_*`
      assert the arm. Reduce request via two-call pull (FFI §4).
- B4. **Reducer exports**: `delta_reducer_apply` (Arrow move-in via `from_ffi`, returns
      `Continue`/`Break`), `delta_reducer_finish`, `delta_reducer_free`. Wrap the kernel `ReducerHandle`.
- B5. **Value exports**: `delta_snapshot_version`, `delta_snapshot_schema` (proto bytes via
      `struct_type_to_proto` + the `schema.proto` top-level wrapper, FFI §7.4), `delta_snapshot_scan_sm`
      (picks metadata|data SM by `ScanKind`, visits the predicate once), `delta_scan_plan` (proto
      bytes via `result_plan_to_proto`), `delta_bytes_free`.
- B6. **cbindgen** regenerates the header; confirm the new handles render opaque and exports appear.
- B7. **Rust STRUCTURAL test — REFINED.** A full drive-to-Done in pure Rust would need an in-process
      reduce executor; the DataFusion executor (`drive_to_completion`) *owns* the SM loop and so can't
      slot into our per-reduce hand-back ABI, and the in-kernel executor was removed at M4. Driving to
      completion through our ABI is therefore the **C++ harness's job** (Phase C, DuckDB executes
      reduces — the real target). Phase B's test validates the ABI SHAPE without reduce execution:
      `delta_open_snapshot` → non-null; `delta_sm_next` → valid step; on REDUCE, `delta_sm_reduce_plan`
      bytes **decode into a `ResultPlan` proto** + `delta_sm_take_reducer` non-null; arm-guards
      (`delta_sm_build_scan` on a snapshot SM → null + err); `delta_bytes_free`/`delta_sm_free`
      lifecycle. Proves surface + proto emission + guards; full drive proven in Phase C.

**Exit:** new ABI compiles, round-trips in Rust, `kdf_*` path untouched → parity still green. Commit
+ push kernel.

---

## Phase C — C++ SDK: the nice API over the new ABI (additive) — ✅ DONE (kernel `95b7829bc`)

**Outcome:** the nice C++ API is `ffi/include/delta_kernel.hpp` (NOT `_sdk` — "SDK" is the module's
nickname, not the filename). Full surface: `open_snapshot` → `StateMachine<T>` (`next`/`submit`/
`submit(EngineError)`/`build`) → `Snapshot` (`version`/`schema`/`scan_sm`/`scan`) → `Scan` (`plan`),
plus `Engine::execute_to_arrow`, `drive`, `Reducer`/`FinishedReducer`, `DeltaError`(thrown)/
`EngineError`(value). Parsed protos out (C++17 header). Compiles clean (`g++ -fsyntax-only`) against
the real generated `delta_kernel_ffi.hpp` + proto `.pb.h`. `build.rs` ships BOTH `delta_kernel.hpp`
(new) and `delta_kernel_sdk.hpp` (legacy, byte-identical to HEAD — still drives the live SQL path).
Additive: the live `sm_sdk.hpp`→`DriveScan` path is untouched → parity intact. Drive-to-completion
deferred to Phase E (DuckDB-only; **DataFusion explicitly disallowed** as a test engine).

**Goal:** `delta_kernel_sdk.hpp` realizes the `SDK_API_DESIGN.md` surface, validated standalone. The
extension still ships the SQL path.

- C1. **Rewrite `delta_kernel_sdk.hpp`** to the target API: `SnapshotBuilder`, `StateMachine<T>`
      (`next`/`submit(EngineResult)`/`submit(EngineError)`/`build`), `Engine::execute_to_arrow`,
      `drive`, `Snapshot`, `Scan`, `Reducer`/`FinishedReducer`, `EngineRequest`/`EngineResult`
      variants, `DeltaError`(thrown)/`EngineError`(value). Parsed-proto getters → C++17 header.
      Replaces today's `KernelString`/`KernelBytes`/`ScanStateMachine` shapes.
- C2. **`FfiSm` build glue**: `next()` assembles `variant<Reduce{plan,reducer}, Done>` from
      `delta_sm_next` + `_reduce_plan` + `_take_reducer`; `build()` dispatches to `_build_snapshot`/
      `_build_scan` via `if constexpr`.
- C3. **Validation — REFINED (DuckDB-only; NO DataFusion).** The only thing that executes reduce
      plans in this architecture is DuckDB; there is no legitimate in-process shortcut, and pulling in
      DataFusion as a test engine is explicitly disallowed (it would reintroduce the cross-engine
      contamination this whole re-arch removes). A drive-to-completion therefore REQUIRES a real
      DuckDB engine that executes IR plans — which IS Phase D (`DeltaPlanBuilder`) + Phase E (wiring).
      So Phase C's honest deliverable is: **the C++ header proven to compile against the real
      generated ABI** (a tiny C++17 TU instantiating the whole surface — `open_snapshot`→
      `StateMachine`→`drive`→`Snapshot`→`scan`→`plan()`, `Engine`, `Reducer` — `g++ -fsyntax-only`
      clean) **and shipped by the build** (`build.rs` copies `delta_kernel.hpp` into ffi-headers).
      The end-to-end drive-to-completion proof lands in **Phase E**, through DuckDB, where it belongs.
- C4. **CMake**: the SDK header + its C++17 island TU (the harness) compile in isolation; the shipping
      extension is unchanged (still C++11, still SQL path). Confirms the C++11/17 seam holds.

**Exit:** SDK drives a real scan end-to-end in a standalone harness; extension parity untouched.
Commit + push kernel (SDK header) + engine (harness/CMake).

---

## Phase D — DeltaPlanBuilder: proto IR → DuckDB (the big one; decomposed)

**Goal:** the engine lowers a `plan::Plan` to DuckDB — this is where the ~950 lines of dialect *leave*
the kernel. Still additive; nothing wired into `delta_scan` yet.

**Decision (locked):** `Lower` produces an **unbound `TableRef`** (not a bound `LogicalOperator`) —
this matches EXACTLY the current integration seam (`delta_scan.cpp` parses the DriveScan SQL into a
`SelectStatement` and returns a `SubqueryRef` via `bind_replace`). The binder plans the `TableRef` to
`LogicalOperator`s. Lowest-friction, and the per-node SQL fallback (already a SELECT string → parse →
`TableRef`) slots in trivially. Hand-built bound `LogicalOperator`s are deferred as a later
optimization.

**The DAG walk (the whole shape).** The IR is an SSA DAG — `Plan.nodes[]`, each with `inputs[]`
(RefIds < its index) and an `output` RefId, `result` = terminal. So:
```
lowered: map<RefId, TableRef>
for node in plan.nodes:              // already topo-ordered
    lowered[node.output] = lower_one(node.op, [lowered[i] for i in node.inputs])
return lowered[plan.result]
```
Each NodeKind lowers in isolation given its already-lowered children → each is an independent step.

### Sub-steps (each independently validatable; stop after any and the tree still works)

> **STATUS (Phase D COMPLETE — committed `45c87c5`).** D0–D8 + S1/E1/E2 all done. All 10 node
> kinds and the full Expression/Predicate/Scalar tree lower to a DuckDB unbound `TableRef`; the
> `delta_plan_builder_test` unit test (a positive case per node + DAG fan-out + error paths) builds
> and passes green against real DuckDB + proto + FFI. Engine-side files:
> `src/functions/delta_scan/delta_plan_builder.{hpp,cpp}` (the DAG walk + node lowering) and
> `delta_proto_lower.{hpp,cpp}` (schema + expression lowering). Two follow-ups surfaced, tracked
> separately: (a) the proto `LoadNode` is missing a `version` field, so time-travel would regress on
> cutover — fix in plan.proto + proto_convert + consume here before Phase F; (b) the DAG walk
> `Copy()`s shared subtrees (correct but potentially redundant for fanned-out nodes) — a CTE-based
> de-dup is a possible future optimization. Next: **Phase E** wires `Lower` into `delta_scan.cpp`
> (whole-plan SQL fallback on `DeltaError`), the DeltaSnapshot adapter, the C++11 façade, and the
> drive-to-completion proof through DuckDB.

- **D0 — the gate.** `DeltaPlanBuilder` skeleton: the DAG walk + `lower_one` dispatch over the
  `Operator` oneof + the `lowered[RefId]` map. Unimplemented arms **throw `DeltaError("unsupported
  node: <kind>")`**. C++17-island TU + a standalone unit test (hand-build a single-node proto →
  assert the produced `TableRef`'s SQL). Exit: compiles + the walk/dispatch unit-tests pass.
  **Fallback granularity — REFINED:** a *per-node* SQL fallback would require re-implementing
  proto→SQL in C++ (exactly what we're removing), so we don't do that. Instead the fallback is at the
  **whole-plan integration point** (Phase E): if `Lower` throws `unsupported`, the caller uses the
  existing DriveScan SQL path for that whole scan. So during D1–D7 a table is served by the new
  TableRef path only once *all* its nodes are implemented; until then it transparently uses SQL.
  Parity therefore never regresses, and the "delete the fallback" step is just removing that
  try/catch in Phase F. D0 validates cheaply via unit tests (no full extension build needed yet).
- **S1** — `SchemaToDuckDB(schema::StructType) → vector<LogicalType>+names` (needed by
  ScanParquet/Values/Project output typing).
- **E1** — `lower_expr(Expression) → ParsedExpression`: Column, Literal (incl Decimal/Timestamp/Date),
  Binary, Unary, Variadic.
- **E2** — remaining expr: If, Struct, ParseJson, MapToStruct, Transform, Opaque/Unknown.
- **D1** — lower `ScanParquet` (→ `read_parquet([files])` TableFunctionRef). (needs S1)
- **D2** — lower `Filter` (→ SubqueryRef + WHERE). (needs E1)
- **D3** — `Project` + `Values`.
- **D4** — `Load` (faithful data read → the `delta_load` TVF ref).
- **D5** — `MaxByVersion` (log-replay dedup → aggregate/window).
- **D6** — `EquiJoin` (left-anti, tombstones) + `UnionAll`.
- **D7** — `ListFiles` + `ScanJson` (commit reads).
- **D8** — full-corpus parity in the harness (proto→TableRef vs SQL path); each node's fallback
  deleted as its real lowering lands and passes.

Every step after D0: replace one node's fallback, validate the workloads using only done nodes,
parity stays green. The old monolithic "D1 spike" = D0+S1+E1+D1+D2.

**Exit:** `DeltaPlanBuilder` lowers the whole corpus at parity, in the harness. Nothing in `delta_scan`
changed yet. Commit + push engine. **D0 gates it.**

---

## Phase E — Wire the proto path into `delta_scan` (behind the SQL path)

**Goal:** the real DuckDB scan operator uses the SDK + `DeltaPlanBuilder`, proven at parity, with the
SQL path still present as the safety net.

- E1. **`DeltaSnapshot` adapter** (engine-side, C++17 island): `Version`/`GetColumns`/`ScanOperator`
      (sync) / `AsyncScan` (→ `DuckDBAsyncDriver`).
- E2. **C++11 façade**: the narrow non-proto surface (`DeltaScanBind` of plain `LogicalType`s +
      opaque `unique_ptr<LogicalOperator>`) so the C++11 extension TUs reach the C++17 island.
- E3. **Metadata path first** (it's the file-list `DriveScan(metadata_only=true)` call sites): route
      through `Snapshot::scan(ScanKind::Metadata)` → plan → `DeltaPlanBuilder`. Run parity.
- E4. **Data path**: the main `delta_scan` operator → `ScanKind::Data` → LogicalOperator. Wire the
      async `DuckDBAsyncDriver` for the BLOCKED/wake path (E1). Run parity + the async concurrency
      harness.
- E5. Full corpus at 1643/1656 through the proto path, SQL path still compiled in.

**Exit:** the extension actually reads via the proto IR path, at parity, async intact. SQL path is now
dead code behind it. Commit + push.

---

## Phase F — Cutover & delete the SQL path

**Goal:** the proto IR path is the only path; the dialect is fully out of the kernel.

- F1. Delete the SQL fallback in `DeltaPlanBuilder` (all NodeKinds covered).
- F2. Delete the engine's `DriveScan`/`RunSqlToArrow` (`sm_sdk.hpp` old form) + the SQL call sites.
- F3. Delete the kernel `_sql` exports (`kdf_sm_reduce_sql`, `kdf_sm_result_sql`, the old `kdf_*` SM
      handles) and **`ffi/src/duckdb/plan_to_sql.rs`** (~950 LOC).
- F4. Final full-corpus parity run (must still be 1643/1656 — now entirely proto/LogicalOperator).
- F5. Docs/PROTOTYPE updated; the three design docs become the as-built reference.

**Exit:** kernel is dialect-pure; DuckDB owns lowering. One read path. Commit + push.

---

## Dependency graph & sizing

```
A (spike, ½d) ──▶ B (kernel ABI, ~2–3d) ──▶ C (C++ SDK + harness, ~2d) ──▶ D (DeltaPlanBuilder, ~1–2wk) ──▶ E (wire in, ~1wk) ──▶ F (cutover, ~2–3d)
```

- **A** gates everything (the `!Send` handle question).
- **D is the critical-path bulk** — the IR→LogicalOperator lowering. Its per-node SQL fallback (D5)
  is what lets D and E overlap safely: E can wire in the moment D covers the nodes a given table
  needs, without waiting for 100% coverage.
- **B/C** are mostly assembly of facilities that already exist (`ReduceSink`, `result_plan_to_proto`,
  `from_ffi`, `ExternResult`, `#[handle_descriptor]`, the generated proto structs).

## Risk register

| Risk | Phase | Mitigation |
|---|---|---|
| `!Send` SM can't be a Handle target | A | Spike first; documented fallback to raw-`*mut` SM handle. |
| C++11/17 ODR abort resurfaces | C, E | SDK/`DeltaPlanBuilder` confined to C++17 island; C++11 reaches it via non-proto façade (Stage-0 lesson). |
| A NodeKind lowers wrong (silent data diff) | D | Per-node parity vs the SQL path, table by table; SQL fallback until a node is proven. |
| Async reducer lifetime across BLOCKED/wake | E | `DuckDBAsyncDriver` holds the moved-out `Reducer` in `optional`; validate with the concurrency harness. |
| Schema proto top-level message missing | B | Confirm `schema.proto` has a top-level `Schema` (FFI §7.4); add if absent. |

## What "done" looks like

`kernel/src/` untouched; `ffi/src/duckdb/` = proto-only `delta_*` ABI + `FfiSm`; `plan_to_sql.rs`
gone; `delta_kernel_sdk.hpp` = the nice API; duckdb-delta reads via `DeltaPlanBuilder` sync + async;
corpus at 1643/1656 the whole way through. The kernel says *what*; DuckDB says *how*.
