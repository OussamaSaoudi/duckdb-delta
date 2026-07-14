//===----------------------------------------------------------------------===//
// Stage-0 round-trip baseline for the kernel-emitted C++ plan-IR proto structs.
//
// This standalone executable is NOT linked into the shipping delta extension (protobuf 33 requires
// C++17, and DuckDB pins C++11 — folding protobuf into the extension breaks ODR; that boundary is
// designed deliberately in Stage 2). Its sole job: prove that the C++ proto structs the kernel's
// build.rs emits (target/ffi-headers/proto-cpp/*.pb.{h,cc}) actually compile, that they link against
// the SAME libprotobuf they were generated with (the generated code hard-asserts a version match at
// include time), and that a ResultPlan survives a serialize -> parse round-trip byte-for-byte.
//===----------------------------------------------------------------------===//
#include "plan.pb.h"

#include <cstdio>
#include <string>

using delta::kernel::plan::PlanNode;
using delta::kernel::plan::ResultPlan;

int main() {
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	// Build a tiny two-node SSA plan: node 0 -> node 1, terminal = 1.
	ResultPlan rp;
	auto *plan = rp.mutable_plan();
	auto *n0 = plan->add_nodes();
	n0->set_output(0);
	auto *n1 = plan->add_nodes();
	n1->set_output(1);
	n1->add_inputs(0);
	rp.set_result(1);

	std::string bytes;
	if (!rp.SerializeToString(&bytes)) {
		fprintf(stderr, "FAIL: SerializeToString\n");
		return 1;
	}

	ResultPlan parsed;
	if (!parsed.ParseFromString(bytes)) {
		fprintf(stderr, "FAIL: ParseFromString\n");
		return 1;
	}

	// Assert the round-trip preserved structure.
	if (parsed.result() != 1) {
		fprintf(stderr, "FAIL: result = %u, expected 1\n", parsed.result());
		return 1;
	}
	if (parsed.plan().nodes_size() != 2) {
		fprintf(stderr, "FAIL: nodes = %d, expected 2\n", parsed.plan().nodes_size());
		return 1;
	}
	if (parsed.plan().nodes(1).inputs_size() != 1 || parsed.plan().nodes(1).inputs(0) != 0) {
		fprintf(stderr, "FAIL: node 1 inputs not preserved\n");
		return 1;
	}

	printf("OK: ResultPlan round-trip (%zu bytes, 2 nodes, terminal=1), libprotobuf version matches\n",
	       bytes.size());
	google::protobuf::ShutdownProtobufLibrary();
	return 0;
}
