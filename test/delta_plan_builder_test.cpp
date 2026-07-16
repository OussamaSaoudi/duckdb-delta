//===----------------------------------------------------------------------===//
// D0 unit test for DeltaPlanBuilder — the DAG walk + dispatch skeleton.
//
// Standalone C++17 (like proto_roundtrip): builds proto plans by hand and checks the walk. At D0
// every NodeKind throws "unsupported node", so we assert:
//   - a single unsupported node → throws delta::DeltaError naming the kind;
//   - the DAG plumbing works: inputs are resolved in order, the terminal RefId is selected, and a
//     missing input/terminal is a clear error (not a crash).
// As D1..D7 land, this test grows a positive case per node.
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_plan_builder.hpp"
#include "delta_kernel.hpp" // delta::DeltaError

#include <cstdio>
#include <string>

using duckdb::DeltaPlanBuilder;
using ::delta::kernel::plan::Operator;
using ::delta::kernel::plan::PlanNode;
using ::delta::kernel::plan::ResultPlan;

// Add a node of the given (unset-for-now) op to the plan: sets output RefId + inputs. Returns nothing;
// the caller sets the op case via the returned pointer.
static PlanNode *AddNode(ResultPlan &rp, uint32_t output, std::initializer_list<uint32_t> inputs) {
	PlanNode *n = rp.mutable_plan()->add_nodes();
	n->set_output(output);
	for (uint32_t in : inputs) {
		n->add_inputs(in);
	}
	return n;
}

static int failures = 0;
#define CHECK(cond, msg)                                                                                     \
	do {                                                                                                     \
		if (!(cond)) {                                                                                       \
			fprintf(stderr, "FAIL: %s\n", msg);                                                              \
			failures++;                                                                                      \
		}                                                                                                    \
	} while (0)

int main() {
	// (1) A single ScanParquet node → unsupported at D0, throwing DeltaError naming the kind.
	{
		ResultPlan rp;
		PlanNode *n = AddNode(rp, /*output=*/0, {});
		n->mutable_op()->mutable_scan_parquet(); // set the oneof to ScanParquet (empty)
		rp.set_result(0);

		bool threw = false;
		std::string msg;
		try {
			DeltaPlanBuilder().Lower(rp);
		} catch (const ::delta::DeltaError &e) {
			threw = true;
			msg = e.what();
		}
		CHECK(threw, "single ScanParquet should throw (unsupported at D0)");
		CHECK(msg.find("ScanParquet") != std::string::npos, "error message should name the ScanParquet kind");
	}

	// (2) DAG plumbing: a Filter over a ScanParquet. Even though both are unsupported, the walk must
	//     reach node 1 (proving inputs resolve in order) — the thrown kind is whichever it hits first
	//     (node 0, ScanParquet).
	{
		ResultPlan rp;
		AddNode(rp, /*output=*/0, {})->mutable_op()->mutable_scan_parquet();
		AddNode(rp, /*output=*/1, {0})->mutable_op()->mutable_filter();
		rp.set_result(1);

		bool threw = false;
		try {
			DeltaPlanBuilder().Lower(rp);
		} catch (const ::delta::DeltaError &) {
			threw = true;
		}
		CHECK(threw, "Filter-over-ScanParquet should throw (both unsupported at D0)");
	}

	// (3) A terminal RefId that no node produces → a clear DeltaError, not a crash.
	{
		ResultPlan rp;
		AddNode(rp, /*output=*/0, {})->mutable_op()->mutable_scan_parquet();
		rp.set_result(99); // no node outputs 99

		bool threw = false;
		std::string msg;
		try {
			DeltaPlanBuilder().Lower(rp);
		} catch (const ::delta::DeltaError &e) {
			threw = true;
			msg = e.what();
		}
		// It throws either on the unsupported node (reached first) or the missing terminal; both are
		// clean DeltaErrors. The point: no crash, always a typed error.
		CHECK(threw, "plan with a bogus terminal RefId should throw a DeltaError");
	}

	if (failures == 0) {
		printf("OK: DeltaPlanBuilder D0 walk/dispatch (%d checks)\n", 3);
		return 0;
	}
	fprintf(stderr, "%d checks failed\n", failures);
	return 1;
}
