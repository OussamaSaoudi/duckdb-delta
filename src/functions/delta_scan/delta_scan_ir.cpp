//===----------------------------------------------------------------------===//
// delta_scan_ir.cpp — the C++17 island behind delta_scan_ir.hpp.
//
// Drives the kernel scan state machine to its terminal ResultPlan and lowers it to a DuckDB
// `TableRef` with DeltaPlanBuilder — the proto-IR scan path. Each Reduce the SM needs is executed by
// DuckDBEngine::execute_to_arrow via the SAME IR lowering (DeltaPlanBuilder), so there is one
// translation path. If a node isn't lowerable yet, it falls back to the DriveScan SQL path (still a
// TableRef), so the C++11 caller never sees an "unsupported node" error.
//
// This TU is compiled at c++17 in isolation (it includes the proto .pb.h via delta_kernel.hpp /
// DeltaPlanBuilder) and linked into the c++11 extension as an OBJECT library. It must expose ONLY the
// non-proto surface declared in delta_scan_ir.hpp.
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_scan_ir.hpp"

#include "functions/delta_scan/delta_plan_builder.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp" // DeltaMultiFileColumnDefinition
#include "delta_utils.hpp"                                   // PredicateVisitor (ffi::EnginePredicate)

// The engine consumes a locally-patched copy of the generated FFI header; point delta_kernel.hpp at it.
#define DELTA_KERNEL_FFI_HEADER "generated_delta_kernel_ffi.hpp"
#include "delta_kernel.hpp" // delta::open_snapshot / drive / Engine / ScanKind / DeltaError

#include "functions/delta_scan/sm_sdk.hpp" // delta_sdk::DriveScan (the SQL fallback) + RunSqlToArrow shape

#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

namespace {

//! Wrap a lowered TableRef in `SELECT * FROM (<ref>)` as a runnable SelectStatement (a plan the
//! DeltaPlanBuilder produces is a relation; to execute it we select from it).
unique_ptr<SelectStatement> SelectStarFrom(unique_ptr<TableRef> ref) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(ref);
	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(node);
	return stmt;
}

//! Export a whole DuckDB query result as one Arrow C-Data batch into the kernel's (ABI-identical)
//! FFI_Arrow* out-params (ownership transfers to the kernel). Mirrors delta_sdk::RunSqlToArrow, but
//! runs a SelectStatement (from a lowered TableRef) rather than a SQL string.
bool RunStatementToArrow(ClientContext &context, unique_ptr<SelectStatement> stmt,
                         ffi::FFI_ArrowArray *out_array, ffi::FFI_ArrowSchema *out_schema) {
	Connection con(*context.db);
	auto result = con.Query(std::move(stmt));
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

//! The engine the kernel drives: a pure ResultPlan -> Arrow executor. It lowers the plan with the
//! SAME DeltaPlanBuilder used for the terminal scan plan (one translation path), runs it in DuckDB,
//! and yields the whole result as a single Arrow batch (the reduces are small metadata relations).
class DuckDBEngine : public delta::Engine {
public:
	explicit DuckDBEngine(ClientContext &context) : context_(context) {}

	delta::ArrowStream execute_to_arrow(const delta::plan::ResultPlan &plan) override {
		// Lower now (throws delta::DeltaError on an unsupported node — surfaces as an engine failure the
		// SM records); stream the single batch lazily so the SDK's apply_all loop drives it.
		DeltaPlanBuilder builder;
		auto ref = builder.Lower(plan);
		auto stmt = SelectStarFrom(std::move(ref));
		auto shared_stmt = std::make_shared<unique_ptr<SelectStatement>>(std::move(stmt));
		auto done = std::make_shared<bool>(false);
		ClientContext &ctx = context_;
		return [shared_stmt, done, &ctx]() -> std::optional<delta::ArrowBatch> {
			if (*done) {
				return std::nullopt;
			}
			*done = true;
			delta::ArrowBatch batch {};
			if (!RunStatementToArrow(ctx, std::move(*shared_stmt), &batch.array, &batch.schema)) {
				throw delta::DeltaError("delta_scan IR engine: failed to execute reduce plan in DuckDB");
			}
			return batch;
		};
	}

private:
	ClientContext &context_;
};

//! Drive the scan SM via the IR path and lower the terminal ResultPlan to a TableRef.
unique_ptr<TableRef> BuildViaIR(const string &path, int64_t version, DeltaScanIRKind kind,
                                ffi::EnginePredicate *predicate, ClientContext &context) {
	DuckDBEngine engine(context);
	auto snap_sm = delta::open_snapshot(path, version);
	delta::Snapshot snapshot = delta::drive(std::move(snap_sm), engine);
	delta::ScanKind sk = (kind == DeltaScanIRKind::Metadata) ? delta::ScanKind::Metadata : delta::ScanKind::Data;
	delta::Scan scan = snapshot.scan(engine, sk, predicate);
	delta::plan::ResultPlan result = scan.plan();
	DeltaPlanBuilder builder;
	return builder.Lower(result);
}

//! The DriveScan SQL fallback: lower the whole scan to SQL and parse it into a SubqueryRef. Used when
//! the IR path throws (e.g. a node DeltaPlanBuilder can't lower yet), so parity never regresses.
unique_ptr<TableRef> BuildViaSql(const string &path, int64_t version, DeltaScanIRKind kind,
                                 ffi::EnginePredicate *predicate, ClientContext &context) {
	string sql = delta_sdk::DriveScan(path, version, kind == DeltaScanIRKind::Metadata, context, predicate);
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw IOException("delta_scan IR fallback: expected a single SELECT statement from plan lowering");
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

} // namespace

unique_ptr<TableRef> BuildDeltaScanRef(const string &path, int64_t version, DeltaScanIRKind kind,
                                       const vector<DeltaMultiFileColumnDefinition> &columns,
                                       optional_ptr<const TableFilterSet> filters, ClientContext &context) {
	// Build the kernel data-skipping predicate from the pushed-down filters, here in the island (so no
	// ffi:: type crosses the facade). The kernel visits it ONCE synchronously inside the scan-SM open,
	// so the visitor need only outlive this call.
	PredicateVisitor visitor(columns, filters);
	ffi::EnginePredicate *predicate = nullptr;
	if (filters && !filters->filters.empty()) {
		predicate = &visitor;
	}

	// Optional diagnostics: set DELTA_SCAN_IR_TRACE=1 to log which path served each scan (IR vs SQL
	// fallback). Off by default; used to verify E3 wiring in dev.
	const bool trace = std::getenv("DELTA_SCAN_IR_TRACE") != nullptr;
	try {
		auto ref = BuildViaIR(path, version, kind, predicate, context);
		if (visitor.error_data.HasError()) {
			throw IOException("delta_scan: predicate translation failed for '%s': %s", path,
			                  visitor.error_data.Message());
		}
		if (trace) {
			fprintf(stderr, "[delta_scan_ir] IR path served '%s' (kind=%s)\n", path.c_str(),
			        kind == DeltaScanIRKind::Metadata ? "metadata" : "data");
		}
		return ref;
	} catch (const delta::DeltaError &e) {
		if (trace) {
			fprintf(stderr, "[delta_scan_ir] SQL FALLBACK for '%s' (kind=%s): %s\n", path.c_str(),
			        kind == DeltaScanIRKind::Metadata ? "metadata" : "data", e.what());
		}
		// A node the IR path can't lower yet — fall back to the SQL path for the whole scan (still a
		// TableRef). Phase F removes this catch once all NodeKinds are covered.
		auto ref = BuildViaSql(path, version, kind, predicate, context);
		if (visitor.error_data.HasError()) {
			throw IOException("delta_scan: predicate translation failed for '%s': %s", path,
			                  visitor.error_data.Message());
		}
		return ref;
	}
}

} // namespace duckdb
