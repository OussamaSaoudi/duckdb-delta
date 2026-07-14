//===----------------------------------------------------------------------===//
// delta::delta_sdk — DuckDB drives the kernel scan state machine.
//
// The kernel hands us a steppable state machine (ffi::kdf_scan_open); DuckDB owns the loop. We pull
// the next step (kdf_sm_get_step); when the kernel needs a Reduce computed, it gives us SQL
// (kdf_sm_reduce_sql) which WE run in DuckDB, then hand the Arrow result back (kdf_sm_submit_reduce).
// When the SM is Done, kdf_sm_result_sql lowers the terminal ResultPlan to the SQL DuckDB executes.
// No callback is ever passed into the kernel — the kernel is passive, the engine drives it.
//
// The raw `kdf_*` C ABI is wrapped by the kernel-shipped RAII SDK (delta_kernel_sdk.hpp): the driver
// below trades in ScanStateMachine / KernelString rather than owning pointers + error strings.
//===----------------------------------------------------------------------===//
#pragma once

#include "delta_utils.hpp" // generated_delta_kernel_ffi.hpp (ffi::) + duckdb common

// The engine consumes a locally-patched copy of the generated FFI header
// (generated_delta_kernel_ffi.hpp), so point the kernel-shipped SDK at that name before including it.
#define DELTA_KERNEL_SDK_FFI_HEADER "generated_delta_kernel_ffi.hpp"
#include "delta_kernel_sdk.hpp" // kernel-shipped RAII adapters: ScanStateMachine, KernelString, ...

#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace duckdb {
namespace delta_sdk {

//! Run `sql` in DuckDB on `context`'s database and export the entire result as one Arrow C Data
//! batch into the kernel's (ABI-identical) FFI_Arrow* out-params (ownership transfers to the
//! kernel). This is how DuckDB executes a kernel Reduce step. Returns false on query error.
inline bool RunSqlToArrow(ClientContext &context, const string &sql, ffi::FFI_ArrowArray *out_array,
                          ffi::FFI_ArrowSchema *out_schema) {
	// Fresh connection on the same database: a separate transaction independent of the outer bind.
	Connection con(*context.db);
	auto result = con.Query(sql);
	if (!result || result->HasError()) {
		return false;
	}
	ClientProperties props = context.GetClientProperties();
	ArrowSchema schema;
	ArrowConverter::ToArrowSchema(&schema, result->types, result->names, props);
	ArrowAppender appender(result->types, STANDARD_VECTOR_SIZE, props, {});
	while (auto chunk = result->Fetch()) {
		if (chunk->size() == 0) {
			break;
		}
		appender.Append(*chunk, 0, chunk->size(), chunk->size());
	}
	ArrowArray array = appender.Finalize();
	*reinterpret_cast<ArrowSchema *>(out_schema) = schema;
	*reinterpret_cast<ArrowArray *>(out_array) = array;
	return true;
}

//! Drive the kernel scan state machine to completion and return the data-stage DuckDB SQL.
//! `metadata_only` selects the kernel's metadata-only scan SM (file-list terminal) vs the full
//! data+metadata SM. DuckDB owns the loop and executes every Reduce.
//!
//! `predicate`, if non-null, is a data-skipping predicate (an `ffi::EnginePredicate`, e.g. built
//! from a `PredicateVisitor` over the pushed-down `TableFilterSet`) the kernel applies to the scan
//! builder so it emits its stats-based file-skip filter. The kernel visits it ONCE, synchronously,
//! inside `kdf_scan_open` — so the engine state it borrows (the `TableFilterSet`) need only outlive
//! THIS call. When null, no data-skipping predicate is applied.
namespace {

//! Drive a reduce-bearing SM (Snapshot or Scan) to Done, executing each Reduce's SQL in DuckDB and
//! handing the Arrow result back. Shared by the snapshot and scan phases below — both expose the same
//! GetStep/ReduceSql/SubmitReduce surface via the RAII SDK.
template <class Sm>
inline void DriveToDone(Sm &sm, ClientContext &context) {
	while (sm.GetStep() == delta_kernel::sdk::Step::Reduce) {
		auto reduce_sql = sm.ReduceSql();
		ffi::FFI_ArrowArray array {};
		ffi::FFI_ArrowSchema schema {};
		if (!RunSqlToArrow(context, reduce_sql.str(), &array, &schema)) {
			throw IOException("delta SM SDK: failed to execute reduce SQL in DuckDB");
		}
		sm.SubmitReduce(&array, &schema);
	}
}

} // namespace

inline string DriveScan(const string &path, int64_t version, bool metadata_only, ClientContext &context,
                        optional_ptr<ffi::EnginePredicate> predicate = nullptr) {
	// Two kernel SMs, driven through the kernel-shipped RAII SDK: first build the point-in-time
	// Snapshot, then build a Scan off it and drive that to its ResultPlan. Each handle frees itself on
	// scope exit (incl. on any thrown KernelException), and every call turns the ABI's out_err into an
	// exception — no manual raw-pointer or try/catch bookkeeping.
	auto snapshot = delta_kernel::sdk::Snapshot::Open(path, version);
	DriveToDone(snapshot, context);
	auto scan = snapshot.Scan(metadata_only, predicate.get());
	DriveToDone(scan, context);
	return scan.ResultSql().str();
}

} // namespace delta_sdk
} // namespace duckdb
