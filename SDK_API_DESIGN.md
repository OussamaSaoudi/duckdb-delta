# Delta kernel SDK — desired C++ API (design)

Status: **design, not implemented.** This captures the API we converged on for the kernel-shipped
C++ SDK (Stage 1.5+), the decisions behind it, and how it maps onto the kernel/ABI. It supersedes
the shape currently in `ffi/include/delta_kernel_sdk.hpp` (the `ScanStateMachine` / `Snapshot`+`Scan`
handles) — that was a stepping stone; this is the target.

The overriding goal: **a nice C++ API.** Pure C++ types out (parsed protos, `std::string`,
values) — no leaked kernel handles, no `_free`, no `Kernel*` wrapper types, no SQL, and the word
"reduce" never appears in the engine's vocabulary.

---

## Mental model

The read path is a sequence of **state machines** that the engine drives, each yielding an inert
**value**:

```
SnapshotBuilder ─.snapshot_sm()─▶ StateMachine<Snapshot> ─drive─▶ Snapshot
                                                                    │
                                              .scan_sm(kind,pred) ──┘
                                                                    ▼
                                          StateMachine<Scan> ─drive─▶ Scan ─▶ plan()
```

- **State machines** are drivable and reduce-bearing. One uniform `StateMachine<T>`; `T` is the
  value it builds.
- **Values** (`Snapshot`, `Scan`) are inert — they answer questions and (for `Snapshot`) make more
  state machines. They never step and never carry reduces.
- **The engine** is a pure IR-plan→Arrow executor. It knows nothing of reducers or state machines.
- **Reduces** are a kernel concept. They live only on the `Reducer` the SM hands out (in a `Reduce`
  request); `drive` feeds it the engine's Arrow output behind the engine's back — the `Engine` itself
  never sees a reducer.

Uniform access rule — everywhere that produces a driven artifact offers **both**:
- `x_sm(...) -> StateMachine<T>` — hand you the machine; you drive it (the async/BLOCKED path).
- `x(Engine&, ...) -> T` — driven internally via `drive(x_sm(...), engine)` (the can-block path).

---

## The API

```cpp
#include "delta/plan.pb.h"     // parsed IR      → makes this a C++17 header
#include "delta/schema.pb.h"

namespace delta {

// ── failure — TWO directional error types ───────────────────────────
// kernel → engine: a KERNEL operation failed (bad log/schema/invariant). THROWN by next/build/getters.
class DeltaError : public std::runtime_error { public: /* kind() */ };
// engine → kernel: the ENGINE couldn't execute a plan. A VALUE the engine produces and hands BACK
// via submit(EngineError) so the kernel can inspect kind() and decide. NOT thrown.
struct EngineError { ErrorKind kind; std::string message; };
enum class ScanKind { Metadata, Data };

// ── arrow ────────────────────────────────────────────────────────────
struct ArrowBatch  { ArrowArray array; ArrowSchema schema; };
using  ArrowStream = std::function<std::optional<ArrowBatch>()>;   // pull next, nullopt at end
enum class Control { Continue, Break };

// ── the reducer sink (kernel reducer handle hidden inside) ───────────
class FinishedReducer {};                        // opaque; only an EngineResult carries it
class Reducer {                                  // move-only, incremental
  Control         apply(const ArrowBatch&);      // feed ONE batch; Break = reducer wants no more
  FinishedReducer finish() &&;                   // consume → finished
  FinishedReducer apply_all(ArrowStream) &&;     // convenience: pull→apply→(Break?)→finish
};

// ── requests & their symmetric results ──────────────────────────────
struct Reduce { const plan::Plan& plan; Reducer reducer; };
struct Done   {};
using  EngineRequest = std::variant<Reduce, Done>;

struct ReduceResult { FinishedReducer reducer; /* room for more fields later */ };
using  EngineResult = std::variant<ReduceResult>;   // one arm per request kind (symmetric)

// ── the engine: a pure IR-plan → Arrow executor. Reduce-agnostic. ────
class Engine {
public:
  virtual ~Engine() = default;
  virtual ArrowStream execute_to_arrow(const plan::Plan&) = 0;
};

// ── the uniform state machine ────────────────────────────────────────
template <class T>                    // T ∈ {Snapshot, Scan}
class StateMachine {
  EngineRequest next();               // advance → Reduce{plan, reducer} | Done  (throws DeltaError)
  void          submit(EngineResult); // engine succeeded → hand back the result
  void          submit(EngineError);  // engine FAILED → kernel observes it, decides how to surface
  T             build() &&;           // consume → value, valid only after Done  (throws DeltaError)
};

// ── drive a state machine to its value via an Engine (blocks) ───────
template <class T>
T drive(StateMachine<T>, Engine&);

// ── entry point ──────────────────────────────────────────────────────
class SnapshotBuilder {
  SnapshotBuilder(std::string path, int64_t version = -1);   // -1 = latest
  StateMachine<Snapshot> snapshot_sm() const;                // async: you drive
  Snapshot               snapshot(Engine&) const;            // sync: drive() inside
};

// ── built snapshot (inert; parsed protos out) ────────────────────────
class Snapshot {
  int64_t                version() const;
  const schema::Schema&  schema() const;                              // parsed proto
  StateMachine<Scan>     scan_sm(ScanKind, const Predicate& = {}) const;
  Scan                   scan(Engine&, ScanKind, const Predicate& = {}) const;
};

// ── built scan (inert; the plan the engine faithfully executes) ─────
class Scan {
  const plan::Plan& plan() const;     // the one plan for this scan's ScanKind
};

}
```

`drive` is the variant match, routing BOTH error directions: a kernel failure (`DeltaError` from
`next`/`build`) propagates out; an engine failure (`EngineError`) is handed *back* to the kernel via
`submit`, never swallowed.
```cpp
template <class T> T drive(StateMachine<T> sm, Engine& engine) {
  for (auto req = sm.next(); ; req = sm.next()) {   // next()/build() may throw DeltaError → propagates
    if (auto* r = std::get_if<Reduce>(&req)) {
      try {
        auto stream = engine.execute_to_arrow(r->plan);
        sm.submit(ReduceResult{ std::move(r->reducer).apply_all(stream) });   // engine ok
      } catch (const EngineError& e) {
        sm.submit(e);                                                          // engine failed → kernel decides
      }
    } else /* Done */ {
      return std::move(sm).build();
    }
  }
}
```

---

## User CUJs (pseudocode, illustrative)

### The engine (shared) — dumb IR-plan → Arrow executor

```cpp
struct DuckDbEngine : delta::Engine {
  ClientContext& ctx;
  delta::ArrowStream execute_to_arrow(const plan::Plan& plan) override {
    auto result = run_plan_in_duckdb(ctx, plan);      // IR → DuckDB query → run
    return [result = std::move(result)]() -> std::optional<delta::ArrowBatch> {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) return std::nullopt;
      return to_arrow_batch(*chunk);
    };
  }
};
```

### A — convenience helpers (sync; can-block path)

```cpp
DuckDbEngine engine{ctx};
delta::Snapshot snap = delta::SnapshotBuilder(path, version).snapshot(engine);
delta::Scan     scan = snap.scan(engine, delta::ScanKind::Data, pred);
const plan::Plan& p  = scan.plan();      // faithfully execute this
execute_plan(ctx, p);
```

Three lines, names no machinery. Two `drive`s happen inside `snapshot()` / `scan()`.

### B1 — lower-level, hand-driven loop (still sync, explicit)

```cpp
auto sm = delta::SnapshotBuilder(path, version).snapshot_sm();
for (;;) {
  delta::EngineRequest req = sm.next();
  if (std::holds_alternative<delta::Done>(req)) { snap = std::move(sm).build(); break; }
  auto& r = std::get<delta::Reduce>(req);       // Reduce{ plan, reducer }
  auto fin = std::move(r.reducer).apply_all(engine.execute_to_arrow(r.plan));
  sm.submit(delta::ReduceResult{ std::move(fin) });
}
```

### B2 — incremental reducer (batch-by-batch, honoring Break)

```cpp
auto& r = std::get<delta::Reduce>(req);
delta::Reducer red   = std::move(r.reducer);
delta::ArrowStream s = engine.execute_to_arrow(r.plan);
while (std::optional<delta::ArrowBatch> b = s())
  if (red.apply(*b) == delta::Control::Break) break;
sm.submit(delta::ReduceResult{ std::move(red).finish() });
```

### B3 — real DuckDB async operator (state held across BLOCKED/wake)

```cpp
struct DeltaScanState {
  delta::StateMachine<delta::Snapshot> sm;
  std::optional<delta::Reducer>        pending_reducer;   // reduce in flight
};

SourceResultType pump(DeltaScanState& st, ClientContext& ctx) {
  if (st.pending_reducer) {                               // sub-pipeline finished → apply now
    auto fin = std::move(*st.pending_reducer).apply_all(take_completed_subpipeline_batches());
    st.pending_reducer.reset();
    st.sm.submit(delta::ReduceResult{ std::move(fin) });
  }
  delta::EngineRequest req = st.sm.next();
  if (std::holds_alternative<delta::Done>(req)) {
    stash_snapshot(std::move(st.sm).build());             // → scan phase drives identically
    return SourceResultType::FINISHED;
  }
  auto& r = std::get<delta::Reduce>(req);
  st.pending_reducer = std::move(r.reducer);
  launch_subpipeline_for(r.plan);                         // async; wakes us
  return SourceResultType::BLOCKED;
}
```

---

## Decisions (with rationale)

| # | Decision | Rationale |
|---|---|---|
| 1 | **One uniform `StateMachine<T>`**, not `SnapshotStateMachine`/`ScanStateMachine` | The drive surface is identical for every SM; only the built value differs. Same as the kernel's `CoroutineSM<R>`. |
| 2 | **`next() -> EngineRequest`**, not separate `Advance`/`Reduce`/`Run` | The request *carries* what's needed. Mirrors the kernel's `get_step() -> EngineRequest`. |
| 3 | **`EngineRequest = std::variant<Reduce, Done>`** (and `EngineResult = variant<ReduceResult>`, symmetric) | Sum types; adding a request kind is a compile error at every match site, not a silent fallthrough. SchemaQuery stays kernel-internal so C++ sees only Reduce/Done. |
| 4 | **Values are inert; `_sm()`/`(engine)` pair everywhere** | `Snapshot`/`Scan` answer questions; you either take the SM (async) or the driven value (sync). Reduces live *only* on the SM. |
| 5 | **`scan(ScanKind, pred)` picks the plan** (metadata vs data) | Maps onto today's two-SM kernel structure → **no kernel change**. The built `Scan` exposes one `plan()`. (An alternative — one Scan emits both plans — would need the SM to reconcile with the superset partition schema; deferred.) |
| 6 | **Getters return parsed proto structs** (`const plan::Plan&`, `const schema::Schema&`), parsed once at `build()` | Nicest to consume. Consequence: the SDK header `#include`s `.pb.h` → it's a **C++17 header** (see boundary note). |
| 7 | **Proto-only; no `Sql()` anywhere** | Legacy debt doesn't belong in a new interface. Migration incrementalism lives at a different layer: the **old SQL driver stays running untouched** until Stage 2 wires the proto path in, then Stage 4 deletes it. Two ABIs in parallel for a stage beats a vestigial `Sql()` on every handle. |
| 8 | **`Engine` is reduce-agnostic: `execute_to_arrow(plan) -> ArrowStream`** | The engine is a pure IR-plan→Arrow executor — same verb it uses to run the terminal `scan.plan()`. "Reduce" is a kernel word; it must not appear in the engine's vocabulary. |
| 9 | **`Reducer` is incremental: `apply(const&) -> Control` + `finish()`, plus `apply_all(stream)`** | The reduce genuinely consumes a *stream* of batches with `Break` short-circuit. `apply_all` is the convenience for the common case; `apply`/`finish` for batch-level control and cross-wake streaming. The kernel reducer handle is hidden inside. |
| 10 | **`FinishedReducer` distinct from `Reducer`** | Type system enforces apply-before-submit — you can't submit an un-applied reducer (compile error, not runtime). "Define errors out of existence." |
| 11 | **`apply(const ArrowBatch&)` borrows** | Caller keeps ownership; kernel copies what it needs. (Note: today's `submit_reduce` moves; borrowing is a deliberate ergonomics choice for the new API — the ABI copies at the boundary.) |
| 12 | **`build() &&` (rvalue-qualified)** | Consuming the SM to produce the value; you can't reuse a drained SM. |
| 13 | **`SnapshotBuilder` entry type** (not free functions / `Table`) | A builder that holds (path, version) and exposes the `_sm()`/`(engine)` pair, symmetric with `Snapshot::scan*`. |
| 14 | **`EngineResult` a variant, extensible** (`ReduceResult` may gain fields; more arms later) | Symmetric with `EngineRequest`; room for non-reduce request results and extra per-result data (e.g. metrics) without breaking the `submit` signature. |

---

## How it maps onto the kernel / C ABI

The full backing — handles, exports, and the source-verified proof that the abstraction holds — is in
`SDK_FFI_INVESTIGATION.md`. In brief:

- **`StateMachine<T>`** → one opaque handle `enum FfiSm { Snapshot(StateMachine<Snapshot>),
  Scan(StateMachine<ResultPlan>) }` (a runtime tag recovering the type C erases). `next`/`submit` are
  `T`-agnostic; `build()` calls the `T`-specific `delta_sm_build_snapshot`/`_scan` (mismatched arm →
  `KernelError`) — **no result-union**, the C++ template keeps `T`.
- **`Reducer`** → the kernel `ReducerHandle` (`apply → Control{Continue|Break}`, `finish`), moved out
  of the SM by `delta_sm_take_reducer`, handed back finished by `delta_sm_submit`.
- **Values** (`Snapshot`, `Scan`) → shared Arc handles; getters return **proto bytes** across the ABI
  (`delta_snapshot_schema`, `delta_scan_plan`), parsed once at `build()` into immutable members.
- **`scan_sm(ScanKind, pred)`** → `delta_snapshot_scan_sm` picks the metadata-vs-data scan SM.
- **The kernel core is untouched** — its `trait StateMachine { type Result }` already models the
  generic SM; the only FFI-layer adjustment is `unsafe impl Send for FfiSm` (single-owner-cursor
  contract; the SMs are `!Send` genawaiter coroutines). Proto-only — **no `_sql`**.

### The one hard constraint: the C++11/C++17 boundary

Because the value getters return **parsed** protos, the SDK header includes `.pb.h` → any TU that
includes it compiles at **C++17**. DuckDB pins **C++11**, and protobuf/Abseil carry a required
`cxx_std_17` compile-feature that CMake will not lower — so a C++17 TU that *also* builds DuckDB
`LogicalOperator`s reintroduces the `LogicalType::VARCHAR` ODR abort we eliminated in Stage 0.

Therefore this SDK header is consumed **only inside the Stage-2 C++17 decode island** (the TU that
turns proto IR into DuckDB `LogicalOperator`s and owns the `.pb.h`), never by the C++11 extension
TUs. This is not a new constraint — it's exactly where Stage 2 already lives.

---

## Sequencing (so parity never breaks)

1. **Kernel ABI** — the `enum FfiSm` handle (`delta_sm_*` exports), reducer moved out via
   `take_reducer` + handed back via `submit`, `build_snapshot`/`build_scan`, proto-bytes getters.
   Details + source-verified backing in `SDK_FFI_INVESTIGATION.md`.
2. **SDK header** — realize exactly the API above (parsed-proto getters; C++17).
3. **Standalone C++17 validation harness** — drive a real snapshot+scan through the SDK, parse the
   protos, assert (à la `proto_roundtrip`; DV-001-style).
4. **Leave the current green SQL driver** (`sm_sdk.hpp` → `delta_scan.cpp`, `_sql` exports)
   untouched — parity stays **1643/1656** throughout.
5. Stage 2 wires the SDK to `DeltaPlanBuilder` (proto IR → `LogicalOperator`) in the C++17 island;
   Stage 4 deletes the SQL path + `plan_to_sql.rs`.

---

## Open items / deferred

- **`ReduceResult` / `EngineResult` extra fields** — carry only the finished reducer today; the
  variant shape leaves room (metrics, per-request-kind arms) without breaking `submit`.
- **One `Scan` emitting both metadata & data plans** — deferred (would need the scan SM to reconcile
  with the superset partition schema); `scan(ScanKind)` picks one for now.
- **`Snapshot::schema()`** — needs a `kdf`-free `delta_snapshot_schema` proto-bytes export (kernel
  has `Snapshot::schema()` internally; no ABI export yet).
- **Naming of the C ABI exports** — drop `Kdf`/`kdf_` prefix; use the Handle infra's conventions.

---

## Async — the state machine IS the async boundary

The `StateMachine` is **passive**: `next()` hands you a request and stops — it never blocks, schedules,
or waits. *Whether you fulfill the request now (sync) or later (BLOCKED/wake) is entirely the
caller's business.* So the SM is inherently async-agnostic — not by adding an async variant, but by
being passive. That's the whole reason "DuckDB drives" works: the drive cadence (a tight `drive`
loop vs. one step per `GetData`) lives 100% in the caller; the SM is identical either way.

Consequence: the async seam is **`StateMachine` (`next`/`submit`/`build`)**, not `Engine`. `Engine`
only models the *synchronous* case.

### Is there a Rust-like portable async in C++? No.

Rust has a language-level `Future`/`poll`/`async` in std (runtimes are external, the interface
isn't). C++ has no usable equivalent here:

| Mechanism | Reality for us |
|---|---|
| `std::future`/`std::async` (C++11) | Blocking `.get()`, no composition/`.then()`. Useless for a non-blocking operator. |
| C++20 coroutines | A mechanism with **no standard task type**; and we're pinned C++11/17 — unavailable. |
| `std::execution` (P2300) | The real composable async — but **C++26**, not in toolchain. |
| asio/folly/cppcoro | Third-party; a kernel SDK must not force one on every engine. |

**So the SDK exposes NO futures/coroutines.** It ships the synchronous `Engine` + `StateMachine`,
which is complete and toolchain-safe. Async is an engine-side *driving strategy*, not a kernel
abstraction.

### DuckDB's async is poll + waker, not a future

Verified against DuckDB headers:
- `SourceResultType::BLOCKED` + `InterruptState::Callback()` — the only operator-level async. It's
  **control-flow, not a value**: you *return* `BLOCKED` and register a callback (a waker); DuckDB
  re-invokes the operator when it fires. There is no future object to hold or return.
- `PendingQueryResult` — closest to a future, but **poll-based** (`ExecuteTask()` →
  `RESULT_NOT_READY`/`RESULT_READY`) and bound to running a whole SQL query on a `Connection` — it
  produces query results, not an arbitrary `Scan` value.

**Why we can't "return DuckDB's `future<Scan>`":** DuckDB has no `future<T>` for an arbitrary
sub-computation. It offers poll + waker. So the DuckDB-shaped representation of "a scan in progress"
is a pump you call again — `pump() -> variant<Blocked, Scan>` — where the `Scan` "arrives" when a
later `pump()` reaches `Done`, not because a future resolved. There is no `.then()`/`co_await`
composition; DuckDB predates and pins away from C++20 coroutines. `variant<Blocked, Scan>` *is*
DuckDB's `future<Scan>` — there just isn't a type by that name.

### `DuckDBAsyncDriver` yes, `DuckDBAsyncEngine` no

- **No `DuckDBAsyncEngine`.** The `Engine` contract `execute_to_arrow(plan) -> ArrowStream` *returns
  the result* — synchronous by construction; an async engine can't satisfy it without blocking.
  Reshaping it into poll/waker reintroduces the generic async layer we rejected, and it would be an
  interface with exactly one implementor injected into a driver that only ever gets that one — a
  shallow module / speculative generality. `Engine` earns its keep only when engines are
  interchangeable *and blocking* (tests, the standalone harness, the metadata path).
- **Yes `DuckDBAsyncDriver`.** In the async case, "produce the Arrow for this plan" and "can we take
  another SM step" are the *same* DuckDB-scheduler question — splitting them into engine+driver buys
  nothing. They collapse into one DuckDB-specific driver over the **raw `StateMachine` primitives**,
  speaking DuckDB's `InterruptState`/BLOCKED directly. It lives **engine-side (duckdb-delta), NOT in
  the kernel SDK** — putting it in the kernel would drag `InterruptState`/`SourceResultType` into the
  kernel, the async twin of the `plan_to_sql`-in-kernel leak this re-arch exists to kill.

```cpp
// duckdb-delta, engine-side. DuckDB-specific by design; consumes the raw passive StateMachine.
template <class T>
class DuckDBAsyncDriver {
  StateMachine<T>            sm_;
  std::optional<Reducer>     reducer_;     // reduce in flight
  std::optional<SubPipeline> exec_;        // its plan running as a DuckDB task

  // Called once per GetData/wake. BLOCKED, or the built value when Done.
  std::variant<Blocked, T> pump(ExecutionContext& ctx, InterruptState& interrupt) {
    if (exec_) {
      if (!exec_->done()) return Blocked{};                          // still running
      sm_.submit(ReduceResult{ std::move(*reducer_).apply_all(exec_->take_batches()) });
      exec_.reset(); reducer_.reset();
    }
    auto req = sm_.next();
    if (std::holds_alternative<Done>(req)) return std::move(sm_).build();   // Ready
    auto& r  = std::get<Reduce>(req);
    reducer_ = std::move(r.reducer);
    exec_    = launch_subpipeline(ctx, r.plan, interrupt);           // fires interrupt on completion
    return Blocked{};
  }
};
```

DuckDB-specific types (`ExecutionContext`, `InterruptState`, `SubPipeline`, `Blocked`) stay
engine-side; only `StateMachine`/`Reducer`/`plan::Plan` cross in from the SDK.

**Verdict:** keep the kernel SDK sync + passive-SM; build `DuckDBAsyncDriver` (one fused,
raw-SM-based type) in duckdb-delta; skip `DuckDBAsyncEngine`. The passive `StateMachine` is what lets
both the sync `drive` and the DuckDB async driver work without the SM knowing which one it's talking
to.

---

## DuckDB extensions over the SDK (`DeltaSnapshot` & conversions)

These live **engine-side (duckdb-delta)** and **compose** the neutral kernel `delta::Snapshot` — they
never modify it. Their job is precisely the DuckDB↔kernel/proto adaptation the kernel must not
contain. Because they touch parsed protos (`schema::Schema`, `plan::Plan`), they live in the **C++17
decode island**, not the C++11 extension TUs.

### `DeltaSnapshot` — the DuckDB adapter over `delta::Snapshot`

```cpp
// duckdb-delta, C++17 island TU.
class DeltaSnapshot {
  delta::Snapshot snap_;                        // the neutral kernel value; owned
public:
  explicit DeltaSnapshot(delta::Snapshot s) : snap_(std::move(s)) {}

  int64_t Version() const { return snap_.version(); }

  // kernel Schema proto -> DuckDB types (what a TableFunction bind / catalog GetTypes needs)
  void GetColumns(vector<string>& names, vector<LogicalType>& types) const {
    SchemaToDuckDB(snap_.schema(), names, types);
  }

  // the money method: scan -> a DuckDB LogicalOperator (Stage 2). Sync-driven.
  unique_ptr<LogicalOperator> ScanOperator(ClientContext& ctx, delta::ScanKind kind,
                                           optional_ptr<TableFilterSet> filters) const {
    KernelPredicate pred = filters ? ToKernelPredicate(*filters) : KernelPredicate{};
    delta::Scan scan = snap_.scan(DuckDbEngine{ctx}, kind, pred);
    return DeltaPlanBuilder(ctx).Lower(scan.plan());
  }

  // async variant: hand back a primed DuckDB async driver for the BLOCKED/wake path.
  DuckDBAsyncDriver<delta::Scan> AsyncScan(delta::ScanKind kind,
                                           optional_ptr<TableFilterSet> filters) const {
    KernelPredicate pred = filters ? ToKernelPredicate(*filters) : KernelPredicate{};
    return DuckDBAsyncDriver<delta::Scan>{ snap_.scan_sm(kind, pred) };
  }
};
```

### The conversion helpers (the actual dialect knowledge — what leaves the kernel)

```cpp
// kernel Schema proto -> DuckDB. The physical->logical type mapping, one place, engine-side.
void SchemaToDuckDB(const schema::Schema&, vector<string>& names, vector<LogicalType>& types);

// DuckDB pushed-down filters -> the kernel Predicate the SDK's scan() accepts.
KernelPredicate ToKernelPredicate(const TableFilterSet&);

// the Stage-2 core: parsed IR plan -> DuckDB logical tree.
class DeltaPlanBuilder {
  DeltaPlanBuilder(ClientContext&);
  unique_ptr<LogicalOperator> Lower(const plan::Plan&);   // ScanParquet->LogicalGet, Filter->…, etc.
};
```

### Call sites

```cpp
// catalog / bind (schema only):
auto snap = DeltaSnapshot{ delta::SnapshotBuilder(path, v).snapshot(DuckDbEngine{ctx}) };
snap.GetColumns(bind_data.names, bind_data.types);
bind_data.version = snap.Version();

// scan operator (sync file-list path):
auto op = snap.ScanOperator(ctx, delta::ScanKind::Metadata, filters);

// scan operator (async data path, BLOCKED/wake):
state.driver = snap.AsyncScan(delta::ScanKind::Data, filters);
// in GetData:
auto r = state.driver.pump(ctx, interrupt);
if (std::holds_alternative<Blocked>(r)) return SourceResultType::BLOCKED;
state.plan = DeltaPlanBuilder(ctx).Lower(std::get<delta::Scan>(r).plan());
```

### Why this shape

- **`DeltaSnapshot` is a deep module**: `ScanOperator(kind, filters) -> LogicalOperator` hides
  predicate-conversion → SM-drive → IR-lowering behind one call. Callers never see `plan::Plan`,
  reducers, or the drive loop.
- **All DuckDB dialect lives here, none in the kernel** (`SchemaToDuckDB`, `ToKernelPredicate`,
  `DeltaPlanBuilder`) — the "engine owns the dialect" payoff of the whole re-arch.
- **Composes, doesn't inherit/modify**: `delta::Snapshot` stays neutral. Equivalent as free functions
  (`ScanOperator(const delta::Snapshot&, …)`) — `.`-vs-`(` taste, same boundary.

### C++11-façade constraint (same C++11/C++17 seam)

`DeltaSnapshot`'s header names `schema::Schema`/`plan::Plan`, so it **can't be included by C++11
TUs**. The C++11 extension side talks to the island through a thin **non-proto façade** — e.g. a
`DeltaScanBind` of plain `LogicalType`s + an opaque `unique_ptr<LogicalOperator>` — and `DeltaSnapshot`
itself stays inside the C++17 island. Same seam as the SDK header, applied to the adapter.
