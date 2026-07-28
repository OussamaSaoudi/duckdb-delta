//===----------------------------------------------------------------------===//
// delta_scan_ir.hpp — the C++11-safe facade over the proto-IR scan path.
//
// The plan-IR scan path (DeltaPlanBuilder + delta_kernel.hpp + the generated proto .pb.h) is a C++17
// island: protobuf/Abseil advertise a *required* cxx_std_17, which — mixed into the C++11-built
// libduckdb — aborts on the LogicalType::VARCHAR ODR (weak c++17 inline vars vs strong out-of-line
// defs). This header is the narrow seam the C++11 extension TUs (delta_scan.cpp, delta_multi_file_
// list.cpp) call through: it names ONLY types both sides share — DuckDB's `TableRef` (a libduckdb
// type, ABI-safe), primitives, `ClientContext`, `TableFilterSet` — and NO proto. The whole SM drive +
// proto decode + DeltaPlanBuilder::Lower + reduce execution lives behind it in delta_scan_ir.cpp,
// compiled at c++17 as an isolated OBJECT library (the `delta_proto` pattern).
//
// (Note: the raw `ffi::` C ABI is NOT the boundary — it is a plain C header already included by C++11
// extension TUs via delta_utils.hpp. Only the proto `.pb.h` forces c++17, so only proto is confined.)
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/planner/table_filter.hpp" // TableFilterSet
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class TableRef;
class ClientContext;
struct DeltaMultiFileColumnDefinition;

//! Which scan the IR path resolves: the file-list reconciliation (Metadata) or the data read (Data).
//! Mirrors delta::ScanKind, kept proto-free so this header stays C++11-safe.
enum class DeltaScanIRKind { Metadata, Data };

//! Resolve a Delta table scan to the DuckDB relation that reads it, via the proto-IR path: open the
//! snapshot, drive the kernel scan state machine to its terminal ResultPlan (running each Reduce as
//! DuckDB SQL/Arrow through the same IR lowering), and lower that plan to an unbound `TableRef` with
//! DeltaPlanBuilder. `version` < 0 means latest.
//!
//! `columns` + `filters` build the kernel data-skipping predicate internally (so no `ffi::` type
//! crosses this seam); pass an empty/absent `filters` for no pushdown.
//!
//! If the IR path cannot lower a node yet (DeltaError "unsupported node"), this transparently falls
//! returns a `TableRef` through the protobuf plan path — so the caller always gets a
//! working relation and never sees an "unsupported" error. Throws only on genuine table/IO failure.
unique_ptr<TableRef> BuildDeltaScanRef(const string &path, int64_t version, DeltaScanIRKind kind,
                                       const vector<DeltaMultiFileColumnDefinition> &columns,
                                       optional_ptr<const TableFilterSet> filters, ClientContext &context);

} // namespace duckdb
