//===----------------------------------------------------------------------===//
// DeltaPlanBuilder — proto plan-IR → DuckDB unbound TableRef (Phase D, D0 skeleton).
//
// D0 is the gate: the SSA-DAG walk + dispatch over the Operator oneof, with every NodeKind arm
// throwing `delta::DeltaError("unsupported node: ...")`. Coverage fills in one node per step
// (D1..D7), each replacing its throw with a real lowering. See SDK_IMPLEMENTATION_PLAN.md.
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_plan_builder.hpp"

#include "delta_kernel.hpp" // delta::DeltaError

#include <string>

namespace duckdb {

using ::delta::kernel::plan::Operator;
using ::delta::kernel::plan::PlanNode;
using ::delta::kernel::plan::ResultPlan;

namespace {

//! Human-readable name for an Operator oneof case (for the unsupported-node error message).
const char *OpCaseName(Operator::OpCase c) {
	switch (c) {
	case Operator::kListFiles:
		return "ListFiles";
	case Operator::kScanParquet:
		return "ScanParquet";
	case Operator::kScanJson:
		return "ScanJson";
	case Operator::kValues:
		return "Values";
	case Operator::kProject:
		return "Project";
	case Operator::kFilter:
		return "Filter";
	case Operator::kLoad:
		return "Load";
	case Operator::kMaxByVersion:
		return "MaxByVersion";
	case Operator::kEquiJoin:
		return "EquiJoin";
	case Operator::kUnionAll:
		return "UnionAll";
	case Operator::OP_NOT_SET:
		return "<unset>";
	default:
		return "<unknown>";
	}
}

} // namespace

unique_ptr<TableRef> DeltaPlanBuilder::Lower(const ResultPlan &result_plan) {
	lowered_.clear();
	const auto &plan = result_plan.plan();

	// The IR is topologically ordered: a node's inputs are RefIds strictly less than its own output,
	// so a single forward pass suffices — each node's inputs are already in `lowered_`.
	for (int i = 0; i < plan.nodes_size(); i++) {
		const PlanNode &node = plan.nodes(i);
		vector<unique_ptr<TableRef>> inputs;
		inputs.reserve(node.inputs_size());
		for (int j = 0; j < node.inputs_size(); j++) {
			uint32_t input_ref = node.inputs(j);
			auto it = lowered_.find(input_ref);
			if (it == lowered_.end()) {
				throw ::delta::DeltaError("DeltaPlanBuilder: input RefId " + std::to_string(input_ref) +
				                          " of node " + std::to_string(node.output()) + " not yet lowered");
			}
			// Move the child out; the SSA DAG uses each output at most... not necessarily once
			// (a node can be a shared input). For D0 we move; multi-consumer sharing (if it arises)
			// is handled when a node with fan-out lands. Copy-on-share would go here.
			inputs.push_back(std::move(it->second));
		}
		lowered_[node.output()] = LowerNode(node, std::move(inputs));
	}

	uint32_t result_ref = result_plan.result();
	auto it = lowered_.find(result_ref);
	if (it == lowered_.end()) {
		throw ::delta::DeltaError("DeltaPlanBuilder: terminal RefId " + std::to_string(result_ref) +
		                          " was not produced by the plan");
	}
	return std::move(it->second);
}

unique_ptr<TableRef> DeltaPlanBuilder::LowerNode(const PlanNode &node, vector<unique_ptr<TableRef>> inputs) {
	const Operator &op = node.op();
	switch (op.op_case()) {
	// D1..D7 replace these throws with real lowerings, one node per step.
	case Operator::kListFiles:
	case Operator::kScanParquet:
	case Operator::kScanJson:
	case Operator::kValues:
	case Operator::kProject:
	case Operator::kFilter:
	case Operator::kLoad:
	case Operator::kMaxByVersion:
	case Operator::kEquiJoin:
	case Operator::kUnionAll:
	case Operator::OP_NOT_SET:
	default:
		throw ::delta::DeltaError(std::string("DeltaPlanBuilder: unsupported node: ") +
		                          OpCaseName(op.op_case()));
	}
}

} // namespace duckdb
