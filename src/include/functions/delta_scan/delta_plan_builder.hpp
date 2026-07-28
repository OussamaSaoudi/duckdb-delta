//===----------------------------------------------------------------------===//
// DeltaPlanBuilder — lower a kernel plan-IR (proto) into a DuckDB unbound TableRef.
//
// The kernel emits the scan as an
// engine-neutral SSA-DAG plan IR (delta::plan::ResultPlan). This builder walks that DAG and lowers
// each node into DuckDB's unbound parse tree — a `unique_ptr<TableRef>` the binder plans exactly like
// a `TableRef` (the integration seam: `delta_scan.cpp` already returns a
// TableRef via bind_replace).
//
// This is the C++17 "decode island": it #includes the generated proto structs (plan.pb.h), so it
// must NOT be included by the C++11 extension TUs — they reach it through a narrow non-proto façade
// (added in Phase E). protobuf pins C++17; mixing with C++11 libduckdb aborts on LogicalType ODR.
//
// Coverage grows one NodeKind at a time (D1..D7). Unimplemented nodes throw
// `delta::DeltaError("unsupported node: ...")`; the caller (Phase E) catches that and falls back to
// unsupported nodes fail explicitly instead of switching execution transports.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/tableref.hpp"

#include "plan.pb.h" // delta::kernel::plan::{Plan, PlanNode, Operator, ResultPlan}

#include <cstdint>
#include <unordered_map>

namespace duckdb {

//! Lowers a kernel plan-IR DAG into a DuckDB unbound `TableRef`. One instance per lowering.
class DeltaPlanBuilder {
public:
	DeltaPlanBuilder() = default;

	//! Lower a full ResultPlan → the TableRef producing the terminal node's rows. Throws
	//! `delta::DeltaError` if any node kind is not yet supported.
	unique_ptr<TableRef> Lower(const ::delta::kernel::plan::ResultPlan &result_plan);

private:
	//! Lower one node given its already-lowered inputs (in `inputs` order) and the schema of its first
	//! input relation (may be null). Dispatches over the `Operator` oneof. Throws on an unsupported kind.
	unique_ptr<TableRef> LowerNode(const ::delta::kernel::plan::PlanNode &node,
	                               vector<unique_ptr<TableRef>> inputs,
	                               const ::delta::kernel::schema::StructType *input_schema);

	//! The output schema of a node (relation column types), needed to lower identity Transforms and
	//! the expressions of downstream Project/Filter/etc. Returns null for nodes whose schema we don't
	//! track (matching the kernel; no Transform is lowered against those). Reads `schemas_` for the
	//! Filter passthrough, so inputs must already be recorded.
	const ::delta::kernel::schema::StructType *NodeOutputSchema(const ::delta::kernel::plan::PlanNode &node) const;

	// SSA scratch: output RefId -> the lowered TableRef for that node. Populated in DAG order.
	std::unordered_map<uint32_t, unique_ptr<TableRef>> lowered_;
	// SSA scratch: output RefId -> that node's output schema (points into the ResultPlan proto, which
	// outlives the walk, or into owned_schemas_ for schemas derived by the engine). Used to thread each
	// node's input relation schema into expression lowering.
	std::unordered_map<uint32_t, const ::delta::kernel::schema::StructType *> schemas_;
	// Load output is file_schema plus its metadata-derived input columns, so it does not exist as one
	// message in the wire plan. Keep those synthesized schemas alive for the duration of the walk.
	std::unordered_map<uint32_t, unique_ptr<::delta::kernel::schema::StructType>> owned_schemas_;
};

} // namespace duckdb
