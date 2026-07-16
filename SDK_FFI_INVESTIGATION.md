# FFI backing for the SDK API

How the kernel C ABI backs the desired C++ API (`SDK_API_DESIGN.md`). Designed from the API down —
**not** from what exists today. Where an existing kernel facility can serve a need, it's noted; but
the design is driven by the API we want, expressed purely in its vocabulary: `StateMachine<Snapshot>`,
`StateMachine<Scan>`, `Snapshot`, `Scan`, `Reducer`. All findings verified against the
delta-kernel-rs prototype branch. Nothing implemented.

---

## 0. The vocabulary (design terms — no implementation nouns)

- **`StateMachine<T>`** — the generic, drivable state machine. `next`/`submit`/`build`; `build → T`.
  The two we use: `StateMachine<Snapshot>` and `StateMachine<Scan>`.
- **`Snapshot` / `Scan`** — the inert values a state machine builds.
- **`Reducer`** — consumes a reduce's Arrow output; `apply`/`finish`.

There is deliberately **no** "driver" / "SmResult" / "MapResult" concept. `StateMachine<T>` is the
whole abstraction; anything it needs internally (footer-read resolution, pending-reduce state) is a
private detail of `StateMachine<T>`, never a separate design entity.

---

## 1. Kernel facilities that can serve the API (verified)

| Need | Kernel facility | Where |
|---|---|---|
| Opaque handles across FFI | `#[handle_descriptor]` + `Handle<H>` — `#[repr(transparent)]` over `NonNull`, `cbindgen:transparent-typedef` → renders opaque. Mutable(Box): `as_mut`/`into_inner`/`drop_handle`; shared(Arc): `as_ref`/`clone_as_arc`. | `ffi/src/handle.rs` |
| The reducer | `ReduceSink::new_handle() -> ReducerHandle`; `ReducerHandle::apply(&ArrowEngineData) -> KdfControl{Continue,Break}`; `finish() -> FinishedHandle`. **Exactly the SDK `Reducer`.** | `kernel/.../ir/nodes.rs`, `plans/kernel_reducers.rs` |
| The generic SM | `trait StateMachine { type Result; get_step()->EngineRequest; submit()->NextStep<Result> }`, impl'd by `CoroutineSM<R>`. **This IS our `StateMachine<T>`.** | `plans/state_machines/framework/` |
| Requests/responses | `EngineRequest::{Reduce{nodes,terminal,sink}, SchemaQuery}`; `EngineResponse::{Reducer(FinishedHandle), Schema, Empty}` | same |
| Snapshot SM / scan SMs | `snapshot_state_machine_for(url,ver,engine) -> CoroutineSM<Snapshot>`; `scan.scan_state_machine()` / `scan_metadata_state_machine() -> CoroutineSM<ResultPlan>` | `plans/state_machines/{snapshot, scan/file_scan}` |
| Snapshot value + accessors | `Snapshot::version()`, `Snapshot::schema() -> SchemaRef`; already a shared handle: `#[handle_descriptor(target=Snapshot, mutable=false)]` | `kernel/src/snapshot/mod.rs`, `ffi/src/lib.rs:692` |
| Plan → proto | `result_plan_to_proto(&ResultPlan)`, `struct_type_to_proto(&StructType)` | `ffi/src/duckdb/proto_convert.rs` |
| Arrow batch import | `from_ffi(FFI_ArrowArray,&FFI_ArrowSchema) -> ArrayData` + `ArrowEngineData` | `delta_kernel::arrow`, `engine::arrow_data` |
| FFI error channel | `ExternResult<T> = Ok(T)|Err(*mut EngineError)` via engine-supplied `AllocateErrorFn` | `ffi/src/error.rs` |
| C++ proto structs | `build.rs` runs `protoc --cpp_out` (Stage 0, done) | `ffi/build.rs` |

**The generic SM, the reducer, plan→proto, and Arrow import all already exist.** The FFI work is
exposing them through the API's handles.

---

## 2. The handles

`StateMachine<T>` is generic in the API, but the C ABI erases `T` (opaque pointer). **Recover the lost
type with a runtime tag** — an enum wrapping the two concrete state machines — not `dyn` erasure:

```rust
// ffi/src/duckdb/ — one opaque SM handle. The arms ARE StateMachine<Snapshot> / StateMachine<Scan>.
enum FfiSm {
    Snapshot(StateMachine<Snapshot>),
    Scan(StateMachine<ResultPlan>),   // ResultPlan is what a scan SM builds; surfaced as `Scan`
}
// SAFETY: single-owner cursor — the engine upholds the Handle contract's no-concurrent-access rule.
// (§6). The impl is on the ONE enum, not per-arm.
unsafe impl Send for FfiSm {}

#[handle_descriptor(target = FfiSm, mutable = true, sized = true)]  pub struct SmHandle;

// values — SHARED (Arc), inert:
//   Snapshot: reuse the EXISTING mainline shared handle (ffi/src/lib.rs:692). No new work.
#[handle_descriptor(target = Scan, mutable = false, sized = true)]  pub struct SharedScan;

// the reducer, in flight (moved out of the SM) and finished:
#[handle_descriptor(target = ReducerHandle,  mutable = true, sized = true)]  pub struct ReducerH;
#[handle_descriptor(target = FinishedHandle, mutable = true, sized = true)]  pub struct FinishedReducerH;
```

Handle count: `SmHandle`, `SharedScan`, `ReducerH`, `FinishedReducerH` + reuse of mainline
`SharedSnapshot`.

---

## 3. Exports (proto-only; `delta_` prefix; errors via §5)

`next`/`submit` are `T`-agnostic → single functions that 2-arm match-forward on `FfiSm`. `build` is
`T`-specific → two exports, each asserting the matching arm. The C++ `StateMachine<T>` template keeps
`T` statically, so `build()` dispatches to the right one via `if constexpr` — **no result-union
needed on either side.**

```
── entry ─────────────────────────────────────────────────────────────
delta_open_snapshot(path, len, version, engine)            -> ExternResult<H<SmHandle>>

── state machine (drives both; next/submit are T-agnostic) ───────────
delta_sm_next(H<SmHandle>, engine)                         -> ExternResult<i32>   // REDUCE | DONE
delta_sm_reduce_plan(H<SmHandle>, *out_len, engine)        -> ExternResult<*u8>   // after REDUCE: proto bytes
delta_sm_take_reducer(H<SmHandle>, engine)                 -> ExternResult<H<ReducerH>>  // after REDUCE
delta_sm_submit(H<SmHandle>, H<FinishedReducerH>, engine)  -> ExternResult<()>
delta_sm_build_snapshot(H<SmHandle>, engine)               -> ExternResult<H<SharedSnapshot>>  // consumes; asserts arm
delta_sm_build_scan(H<SmHandle>, engine)                   -> ExternResult<H<SharedScan>>      // consumes; asserts arm
delta_sm_free(H<SmHandle>)

── reducer (moved out of the SM; incremental) ───────────────────────
delta_reducer_apply(H<ReducerH>, *FFI_ArrowArray, *FFI_ArrowSchema, engine) -> ExternResult<i32/*Control*/>
delta_reducer_finish(H<ReducerH>, engine)                  -> ExternResult<H<FinishedReducerH>>  // consumes
delta_reducer_free(H<ReducerH>)

── snapshot value (reuses mainline SharedSnapshot + free_snapshot) ───
delta_snapshot_version(H<SharedSnapshot>, engine)          -> ExternResult<i64>
delta_snapshot_schema(H<SharedSnapshot>, *out_len, engine) -> ExternResult<*u8>   // Schema proto bytes
delta_snapshot_scan_sm(H<SharedSnapshot>, scan_kind: i32, predicate: *EnginePredicate, engine)
                                                           -> ExternResult<H<SmHandle>>  // picks metadata|data SM

── scan value ────────────────────────────────────────────────────────
delta_scan_plan(H<SharedScan>, *out_len, engine)           -> ExternResult<*u8>   // ResultPlan proto bytes
delta_scan_free(H<SharedScan>)

── proto byte-buffer free ────────────────────────────────────────────
delta_bytes_free(*u8, len)
```

**~15 exports, 4 new handle types.** Proto-only — no `_sql`. Maps 1:1 to the C++ SDK: `next()`
assembles `variant<Reduce{plan,reducer}, Done>` from `delta_sm_next` + `_reduce_plan` +
`_take_reducer`; `build()` calls `_build_snapshot`/`_build_scan` per `T`.

---

## 4. The Reduce request across FFI — two-call pull

`delta_sm_next` returns just the kind (`REDUCE | DONE`). On `REDUCE`, the plan bytes and the reducer
are pulled separately (`delta_sm_reduce_plan`, `delta_sm_take_reducer`), and the C++ `next()` assembles
them into `variant<Reduce{plan, reducer}, Done>`. This avoids a returned struct that owns both a heap
buffer and a handle (awkward through cbindgen), and the reducer is *moved out* of the SM — the engine
owns driving it (critical for the async/BLOCKED path, where it's held across wakes); `submit` puts the
*finished* one back.

---

## 5. Error channel — decision

- **`ExternResult<T>`** (idiomatic; every mainline export uses it) — needs an `AllocateErrorFn`,
  sourced from the `SharedExternEngine` handle, so every export threads `engine` through. Consistent
  with the rest of the kernel FFI.
- **Lightweight `out_err` C-string** — self-contained, no engine handle needed, but a second error
  idiom in the crate.

Recommend `ExternResult` for consistency; the C++ SDK turns a kernel-origin `ExternResult::Err` into
a thrown **`DeltaError`**. Note the *other* direction: an **`EngineError`** (the engine reporting it
couldn't run a plan) flows the opposite way — `submit(EngineError)` hands it back INTO the kernel
(mirroring the kernel's `submit(result: Result<EngineResponse, EngineError>)`), via a `delta_sm_submit_error`
export. Two directional error types, two ABI paths. This
is the one place the ABI signatures carry an `engine` param purely for error allocation.

---

## 6. Does the abstraction hold in the Rust? (verified)

**Generic SM — yes.** `trait StateMachine { type Result }` is a real associated-type generic, impl'd
by `CoroutineSM<R>`. `StateMachine<Snapshot>` / `StateMachine<Scan>` are genuine monomorphizations,
not a fiction. The C++ `StateMachine<T>` mirrors a real Rust generic.

**The one mismatch — `!Send`.** `CoroutineSM` uses `genawaiter2::rc` **deliberately** (documented:
SMs are CPU-only, single-threaded). `rc::Gen` is `Rc`-based → `CoroutineSM<R>` is **`!Send`**.
`HandleDescriptor::Target` is bounded `Send` (`handle.rs:49`) with no escape. So a state machine
can't be a `#[handle_descriptor]` target without asserting `Send`.

**Resolution — `unsafe impl Send for FfiSm`.** `Send` is about moving ownership across threads; the
SM is a single-owner cursor and the `Handle` contract *already* requires the engine to enforce
mutual exclusion ("no mutable handle to more than one call at a time"). The repo sanctions exactly
this: `handle.rs:546` has `NotSync{ptr:*mut u32}` + `unsafe impl Send` behind
`#[handle_descriptor(mutable=true)]`. One `unsafe impl` on the enum; the arms need no change.

**Values — already `Send+Sync`.** `Snapshot` is already a mainline shared Arc handle (`lib.rs:692`);
`Scan`/`ResultPlan` are plain data. Only the *state machine* is `!Send`; values drop straight in.

**Verdict:** the abstraction holds. Kernel core is **untouched** — it already models generic state
machines exactly as the API wants. The FFI work is: the `FfiSm` enum + its one `unsafe impl Send`,
2-arm forwarding for `next`/`submit`, two `build_*` exports, and the value/reducer handles — all in
`ffi/src/duckdb/`.

---

## 7. Constraints & gotchas

1. **`R: 'static`** — `CoroutineSM<R>` requires it; the arms (`Snapshot`, `ResultPlan`) satisfy it.
2. **`!Send` (§6)** — the one `unsafe impl Send for FfiSm`. **Spike this first** (does
   `#[handle_descriptor(mutable=true)]` over the `Send`-wrapped enum compile + pass a real drive?).
3. **Arrow ownership on `apply`** — the API's `apply(const ArrowBatch&)` borrows; `from_ffi` moves
   (replace-with-empty). Either the ABI `delta_reducer_apply` moves (ownership in, structs emptied)
   and the C++ "borrow" is a convenience over it, or the ABI copies. Decide at implementation.
4. **Schema proto top-level** — `struct_type_to_proto` yields a `StructType`; confirm `schema.proto`
   has a top-level `Schema` message (else `delta_snapshot_schema` returns a `StructType` proto).
5. **`scan_builder(Arc<Snapshot>)`** — the shared snapshot handle exposes its `Arc` via
   `clone_as_arc`, so `delta_snapshot_scan_sm` can build a scan SM (and more than one) off it.
6. **cbindgen** — new handle types must be `pub` and reachable from a `pub` export signature to be
   emitted; `Handle<X>` renders as `X *` (transparent-typedef).

---

## 8. Build order

1. **Spike the `!Send` question (§6, §7 item 2)** — the one risky thing: `Send`-wrapped `FfiSm` enum behind `#[handle_descriptor]`,
   driven to completion for a real snapshot. Gates the whole enum-handle ABI.
2. Define the handles (§2) + `FfiSm`; error channel (§5).
3. Exports (§3), two-call reduce pull (§4), reusing the kernel reducer, `result_plan_to_proto`,
   `struct_type_to_proto`, `from_ffi`.
4. `schema_to_proto` wrapper (§7.4).
5. cbindgen regenerates the header; the C++17 SDK (`SDK_API_DESIGN.md`) sits on top.
6. Standalone C++17 harness drives snapshot+scan through the SDK and asserts. **The current SQL path
   stays green (1643/1656) until Stage 2/4 cut over.**
