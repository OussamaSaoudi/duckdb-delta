//===----------------------------------------------------------------------===//
// Unit test for DeltaPlanBuilder — the proto plan-IR -> DuckDB unbound TableRef lowering.
//
// Standalone C++17 (like proto_roundtrip): builds proto plans by hand, lowers them, and asserts on
// the produced TableRef's ToString() (the reconstructed SQL) — a cheap, link-free check that the
// right DuckDB parse tree was built. Covers one positive case per NodeKind (D1..D7) plus the DAG
// walk's error paths. Full end-to-end parity is the acceptance-workloads harness (D8).
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_plan_builder.hpp"
#include "delta_kernel.hpp" // delta::DeltaError

#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/tableref.hpp"

#include <cstdio>
#include <string>

// This test links only duckdb_static (the parser), which we never fully initialize — it builds
// TableRefs and calls ToString(). duckdb_static's DuckDB ctor references ExtensionHelper::
// LoadAllExtensions (normally provided by the generated extension-loader archive, which would drag in
// httpfs -> curl/OpenSSL and crash on the dual-OpenSSL constructor). We never construct a DuckDB here,
// so a no-op stub satisfies the linker without pulling that chain in. See CMakeLists for the rationale.
namespace duckdb {
void ExtensionHelper::LoadAllExtensions(DuckDB &) {
}
} // namespace duckdb

using duckdb::DeltaPlanBuilder;
using duckdb::unique_ptr;
using duckdb::TableRef;
using ::delta::kernel::plan::Operator;
using ::delta::kernel::plan::PlanNode;
using ::delta::kernel::plan::ResultPlan;
namespace kschema = ::delta::kernel::schema;
namespace kexpr = ::delta::kernel::expressions;

static PlanNode *AddNode(ResultPlan &rp, uint32_t output, std::initializer_list<uint32_t> inputs) {
	PlanNode *n = rp.mutable_plan()->add_nodes();
	n->set_output(output);
	for (uint32_t in : inputs) {
		n->add_inputs(in);
	}
	return n;
}

// Add a primitive field to a struct schema.
static void AddField(kschema::StructType *st, const std::string &name, kschema::SimplePrimitiveType prim) {
	auto *f = st->add_fields();
	f->set_name(name);
	f->mutable_data_type()->mutable_primitive()->set_simple(prim);
	f->set_nullable(true);
}

static int failures = 0;
#define CHECK(cond, msg)                                                                                     \
	do {                                                                                                     \
		if (!(cond)) {                                                                                       \
			fprintf(stderr, "FAIL: %s\n", msg);                                                              \
			failures++;                                                                                      \
		}                                                                                                    \
	} while (0)

static bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

// Lower a plan and return the terminal TableRef's ToString() (empty on throw, with `threw` set).
static std::string LowerToString(const ResultPlan &rp, bool &threw) {
	threw = false;
	try {
		auto ref = DeltaPlanBuilder().Lower(rp);
		return ref->ToString();
	} catch (const ::delta::DeltaError &) {
		threw = true;
		return "";
	}
}

int main() {
	using kschema::SIMPLE_PRIMITIVE_TYPE_LONG;
	using kschema::SIMPLE_PRIMITIVE_TYPE_STRING;

	// (1) ScanParquet → read_parquet projection.
	{
		ResultPlan rp;
		auto *scan = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		scan->add_files()->set_location("file:///tmp/t/part-0.parquet");
		AddField(scan->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		rp.set_result(0);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "ScanParquet should lower without throwing");
		CHECK(Contains(sql, "read_parquet"), "ScanParquet should emit read_parquet");
		CHECK(Contains(sql, "part-0.parquet"), "ScanParquet should reference the file path");
		CHECK(Contains(sql, "dkrp_"), "ScanParquet columns should be source-qualified to avoid alias self-reference");
	}

	// (2) ScanParquet with no files → typed empty relation (no read_parquet, a false filter).
	{
		ResultPlan rp;
		auto *scan = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		AddField(scan->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		rp.set_result(0);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "empty ScanParquet should lower");
		CHECK(!Contains(sql, "read_parquet"), "empty ScanParquet should not read files");
	}

	// (3) Filter over ScanParquet → WHERE with a comparison.
	{
		ResultPlan rp;
		auto *scan = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		scan->add_files()->set_location("file:///tmp/t/part-0.parquet");
		AddField(scan->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		// Filter: id > 5
		auto *filt = AddNode(rp, 1, {0})->mutable_op()->mutable_filter();
		auto *bp = filt->mutable_predicate()->mutable_binary();
		bp->set_op(kexpr::BINARY_PREDICATE_OP_GREATER_THAN);
		bp->mutable_left()->mutable_column()->add_path("id");
		bp->mutable_right()->mutable_literal()->set_long_(5);
		rp.set_result(1);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "Filter should lower");
		CHECK(Contains(sql, "WHERE"), "Filter should emit a WHERE clause");
		CHECK(Contains(sql, "> 5") || Contains(sql, ">5") || Contains(sql, "5"), "Filter should reference the literal");
	}

	// (4) Project over ScanParquet → SELECT expr AS name.
	{
		ResultPlan rp;
		auto *scan = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		scan->add_files()->set_location("file:///tmp/t/part-0.parquet");
		AddField(scan->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *proj = AddNode(rp, 1, {0})->mutable_op()->mutable_project();
		auto *ne = proj->add_named_exprs();
		ne->set_name("id2");
		ne->mutable_expr()->mutable_column()->add_path("id");
		AddField(proj->mutable_output_schema(), "id2", SIMPLE_PRIMITIVE_TYPE_LONG);
		rp.set_result(1);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "Project should lower");
		CHECK(Contains(sql, "id2"), "Project should alias to the output name");
	}

	// (5) Values → an inline relation.
	{
		ResultPlan rp;
		auto *vals = AddNode(rp, 0, {})->mutable_op()->mutable_values();
		AddField(vals->mutable_schema(), "path", SIMPLE_PRIMITIVE_TYPE_STRING);
		auto *row = vals->add_rows();
		row->add_values()->set_string("part-0.parquet");
		rp.set_result(0);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "Values should lower");
		CHECK(Contains(sql, "part-0.parquet"), "Values should carry its row literal");
	}

	// (6) Load over a Values input → delta_load table function.
	{
		ResultPlan rp;
		auto *vals = AddNode(rp, 0, {})->mutable_op()->mutable_values();
		AddField(vals->mutable_schema(), "path", SIMPLE_PRIMITIVE_TYPE_STRING);
		vals->add_rows()->add_values()->set_string("part-0.parquet");
		auto *load = AddNode(rp, 1, {0})->mutable_op()->mutable_load();
		load->set_file_type(::delta::kernel::plan::FILE_TYPE_PARQUET);
		load->set_base_url("file:///tmp/t/");
		AddField(load->mutable_file_schema(), "value", SIMPLE_PRIMITIVE_TYPE_LONG);
		load->mutable_file_meta()->mutable_path_column()->add_path("path");
		rp.set_result(1);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "Load should lower");
		CHECK(Contains(sql, "delta_load"), "Load should emit the delta_load table function");
	}

	// (7) MaxByVersion over ScanParquet → arg_max hash aggregate.
	// (7) A Project/Transform after Load sees metadata-derived columns in the Load output schema.
	//     This is the terminal shape of a full data scan: physical file columns plus per-file constants
	//     are transformed into the logical row schema.
	{
		ResultPlan rp;
		auto *vals = AddNode(rp, 0, {})->mutable_op()->mutable_values();
		AddField(vals->mutable_schema(), "path", SIMPLE_PRIMITIVE_TYPE_STRING);
		auto *meta = vals->mutable_schema()->add_fields();
		meta->set_name("fileConstantValues");
		meta->set_nullable(true);
		AddField(meta->mutable_data_type()->mutable_struct_(), "partition", SIMPLE_PRIMITIVE_TYPE_STRING);

		auto *load = AddNode(rp, 1, {0})->mutable_op()->mutable_load();
		load->set_file_type(::delta::kernel::plan::FILE_TYPE_PARQUET);
		load->set_base_url("file:///tmp/t/");
		AddField(load->mutable_file_schema(), "value", SIMPLE_PRIMITIVE_TYPE_LONG);
		load->mutable_file_meta()->mutable_path_column()->add_path("path");
		load->add_metadata_derived_columns()->add_path("fileConstantValues");

		auto *project = AddNode(rp, 2, {1})->mutable_op()->mutable_project();
		auto *named = project->add_named_exprs();
		named->set_name("row");
		named->mutable_expr()->mutable_transform(); // top-level identity transform
		auto *row = project->mutable_output_schema()->add_fields();
		row->set_name("row");
		row->set_nullable(true);
		auto *row_struct = row->mutable_data_type()->mutable_struct_();
		AddField(row_struct, "value", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *row_meta = row_struct->add_fields();
		row_meta->set_name("fileConstantValues");
		row_meta->set_nullable(true);
		AddField(row_meta->mutable_data_type()->mutable_struct_(), "partition", SIMPLE_PRIMITIVE_TYPE_STRING);

		rp.set_result(2);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "Project after Load should see metadata-derived columns in its input schema");
		CHECK(Contains(sql, "fileConstantValues"), "terminal Transform should preserve the derived column");
	}

	// (8) MaxByVersion over ScanParquet → arg_max hash aggregate.
	{
		ResultPlan rp;
		auto *scan = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		scan->add_files()->set_location("file:///tmp/t/part-0.parquet");
		AddField(scan->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_STRING);
		AddField(scan->mutable_schema(), "ver", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *mbv = AddNode(rp, 1, {0})->mutable_op()->mutable_max_by_version();
		mbv->mutable_version_column()->mutable_column()->add_path("ver");
		mbv->add_group_by()->mutable_column()->add_path("id");
		AddField(mbv->mutable_output_schema(), "id", SIMPLE_PRIMITIVE_TYPE_STRING);
		rp.set_result(1);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "MaxByVersion should lower");
		CHECK(Contains(sql, "arg_max"), "MaxByVersion should emit arg_max");
		CHECK(Contains(sql, "GROUP BY"), "MaxByVersion should group");
	}

	// (9) UnionAll of two ScanParquets → UNION ALL BY NAME.
	{
		ResultPlan rp;
		auto *a = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		a->add_files()->set_location("file:///tmp/t/a.parquet");
		AddField(a->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *b = AddNode(rp, 1, {})->mutable_op()->mutable_scan_parquet();
		b->add_files()->set_location("file:///tmp/t/b.parquet");
		AddField(b->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		AddNode(rp, 2, {0, 1})->mutable_op()->mutable_union_all();
		rp.set_result(2);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "UnionAll should lower");
		CHECK(Contains(sql, "UNION ALL BY NAME"), "UnionAll should emit UNION ALL BY NAME");
	}

	// (10) EquiJoin (LeftAnti) → ANTI JOIN ... ON IS NOT DISTINCT FROM.
	{
		ResultPlan rp;
		auto *l = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		l->add_files()->set_location("file:///tmp/t/l.parquet");
		AddField(l->mutable_schema(), "k", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *r = AddNode(rp, 1, {})->mutable_op()->mutable_scan_parquet();
		r->add_files()->set_location("file:///tmp/t/r.parquet");
		AddField(r->mutable_schema(), "k", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *join = AddNode(rp, 2, {0, 1})->mutable_op()->mutable_equi_join();
		join->set_kind(::delta::kernel::plan::JOIN_KIND_LEFT_ANTI);
		join->add_left_keys()->mutable_column()->add_path("k");
		join->add_right_keys()->mutable_column()->add_path("k");
		rp.set_result(2);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "EquiJoin should lower");
		CHECK(Contains(sql, "ANTI"), "EquiJoin(LeftAnti) should emit an ANTI JOIN");
	}

	// (11) DAG fan-out: one node consumed by two parents (union of a node and a filter over it). The
	//      walk must Copy the shared child, not move it (else the second consumer sees null → crash).
	{
		ResultPlan rp;
		auto *scan = AddNode(rp, 0, {})->mutable_op()->mutable_scan_parquet();
		scan->add_files()->set_location("file:///tmp/t/part-0.parquet");
		AddField(scan->mutable_schema(), "id", SIMPLE_PRIMITIVE_TYPE_LONG);
		auto *filt = AddNode(rp, 1, {0})->mutable_op()->mutable_filter(); // consumes 0
		auto *bp = filt->mutable_predicate()->mutable_binary();
		bp->set_op(kexpr::BINARY_PREDICATE_OP_GREATER_THAN);
		bp->mutable_left()->mutable_column()->add_path("id");
		bp->mutable_right()->mutable_literal()->set_long_(0);
		AddNode(rp, 2, {0, 1})->mutable_op()->mutable_union_all(); // consumes 0 AGAIN, and 1
		rp.set_result(2);
		bool threw;
		std::string sql = LowerToString(rp, threw);
		CHECK(!threw, "DAG fan-out (shared child) should lower without crashing");
		CHECK(Contains(sql, "UNION ALL BY NAME"), "fan-out union should still emit the union");
	}

	// (12) Missing terminal RefId → clean DeltaError, not a crash.
	{
		ResultPlan rp;
		AddNode(rp, 0, {})->mutable_op()->mutable_values();
		rp.set_result(99);
		bool threw;
		LowerToString(rp, threw);
		CHECK(threw, "bogus terminal RefId should throw a DeltaError");
	}

	if (failures == 0) {
		printf("OK: DeltaPlanBuilder lowering (all node kinds)\n");
		return 0;
	}
	fprintf(stderr, "%d checks failed\n", failures);
	return 1;
}
