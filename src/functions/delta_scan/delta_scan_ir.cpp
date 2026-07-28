//===----------------------------------------------------------------------===//
// delta_scan_ir.cpp — the C++17 island behind delta_scan_ir.hpp.
//
// Drives the kernel scan state machine to its terminal ResultPlan and lowers it to a DuckDB
// `TableRef` with DeltaPlanBuilder — the proto-IR scan path. Each Reduce the SM needs is executed by
// DuckDBEngine::execute_to_arrow via the SAME IR lowering (DeltaPlanBuilder), so there is one
// translation path. Unsupported nodes are reported directly; there is no alternate SQL transport.
//
// This TU is compiled at c++17 in isolation (it includes the proto .pb.h via delta_kernel.hpp /
// DeltaPlanBuilder) and linked into the c++11 extension as an OBJECT library. It must expose ONLY the
// non-proto surface declared in delta_scan_ir.hpp.
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_scan_ir.hpp"

#include "functions/delta_scan/delta_plan_builder.hpp"
#include "functions/delta_scan/delta_multi_file_reader.hpp" // DeltaMultiFileColumnDefinition
#include "delta_utils.hpp"

// The engine consumes a locally-patched copy of the generated FFI header; point delta_kernel.hpp at it.
#define DELTA_KERNEL_FFI_HEADER "generated_delta_kernel_ffi.hpp"
#include "delta_kernel.hpp" // delta::open_snapshot / drive / Engine / ScanKind / DeltaError

#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

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
//! FFI_Arrow* out-params (ownership transfers to the kernel). It runs a SelectStatement built from
//! the lowered TableRef; no SQL string is parsed or transported.
bool RunStatementToArrow(ClientContext &context, unique_ptr<SelectStatement> stmt,
                         delta::ArrowArray *out_array, delta::ArrowSchema *out_schema) {
	Connection con(*context.db);
	auto result = con.Query(std::move(stmt));
	if (!result || result->HasError()) {
		if (std::getenv("DELTA_SCAN_IR_TRACE") && result) {
			fprintf(stderr, "[delta_scan_ir] reduce query error: %s\n", result->GetError().c_str());
		}
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

std::optional<delta::Expression> ToPredicateLiteral(const Value &value) {
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return delta::Expression::boolean(BooleanValue::Get(value));
	case LogicalTypeId::TINYINT:
		return delta::Expression::int8(value.GetValueUnsafe<int8_t>());
	case LogicalTypeId::SMALLINT:
		return delta::Expression::int16(value.GetValueUnsafe<int16_t>());
	case LogicalTypeId::INTEGER:
		return delta::Expression::int32(value.GetValueUnsafe<int32_t>());
	case LogicalTypeId::BIGINT:
		return delta::Expression::int64(value.GetValueUnsafe<int64_t>());
	case LogicalTypeId::FLOAT:
		return delta::Expression::float32(value.GetValueUnsafe<float>());
	case LogicalTypeId::DOUBLE:
		return delta::Expression::float64(value.GetValueUnsafe<double>());
	case LogicalTypeId::VARCHAR:
		return delta::Expression::string(StringValue::Get(value));
	case LogicalTypeId::DATE:
		return delta::Expression::date(DateValue::Get(value).days);
	case LogicalTypeId::DECIMAL: {
		uint64_t high;
		uint64_t low;
		switch (value.type().InternalType()) {
		case PhysicalType::INT16: {
			auto v = value.GetValueUnsafe<int16_t>();
			high = v < 0 ? UINT64_MAX : 0;
			low = static_cast<uint64_t>(static_cast<int64_t>(v));
			break;
		}
		case PhysicalType::INT32: {
			auto v = value.GetValueUnsafe<int32_t>();
			high = v < 0 ? UINT64_MAX : 0;
			low = static_cast<uint64_t>(static_cast<int64_t>(v));
			break;
		}
		case PhysicalType::INT64: {
			auto v = value.GetValueUnsafe<int64_t>();
			high = v < 0 ? UINT64_MAX : 0;
			low = static_cast<uint64_t>(v);
			break;
		}
		case PhysicalType::INT128: {
			auto v = value.GetValueUnsafe<hugeint_t>();
			high = static_cast<uint64_t>(v.upper);
			low = v.lower;
			break;
		}
		default:
			return std::nullopt;
		}
		return delta::Expression::decimal(high, low, DecimalType::GetWidth(value.type()),
		                                  DecimalType::GetScale(value.type()));
	}
	default:
		return std::nullopt;
	}
}

std::optional<delta::Comparison> ToComparison(ExpressionType comparison) {
	switch (comparison) {
	case ExpressionType::COMPARE_LESSTHAN:
		return delta::Comparison::LessThan;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return delta::Comparison::LessThanOrEqual;
	case ExpressionType::COMPARE_GREATERTHAN:
		return delta::Comparison::GreaterThan;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return delta::Comparison::GreaterThanOrEqual;
	case ExpressionType::COMPARE_EQUAL:
		return delta::Comparison::Equal;
	case ExpressionType::COMPARE_NOTEQUAL:
		return delta::Comparison::NotEqual;
	default:
		return std::nullopt;
	}
}

std::optional<delta::Predicate> ToPredicate(const string &column, const TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant = filter.Cast<ConstantFilter>();
		auto comparison = ToComparison(constant.comparison_type);
		auto literal = ToPredicateLiteral(constant.constant);
		if (!comparison || !literal) {
			return std::nullopt;
		}
		return delta::Predicate::compare(*comparison, delta::Expression::column(column), std::move(*literal));
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		vector<delta::Predicate> children;
		for (auto &child : conjunction.child_filters) {
			auto predicate = ToPredicate(column, *child);
			if (!predicate) {
				return std::nullopt;
			}
			children.push_back(std::move(*predicate));
		}
		return delta::Predicate::all(std::move(children));
	}
	case TableFilterType::IS_NULL:
		return delta::Predicate::is_null(delta::Expression::column(column));
	case TableFilterType::IS_NOT_NULL:
		return delta::Predicate::logical_not(
		    delta::Predicate::is_null(delta::Expression::column(column)));
	case TableFilterType::STRUCT_EXTRACT: {
		auto *current = &filter.Cast<StructFilter>();
		string nested = column + "." + current->child_name;
		const TableFilter *child = current->child_filter.get();
		while (child->filter_type == TableFilterType::STRUCT_EXTRACT) {
			current = &child->Cast<StructFilter>();
			nested += "." + current->child_name;
			child = current->child_filter.get();
		}
		return ToPredicate(nested, *child);
	}
	default:
		return std::nullopt;
	}
}

std::optional<delta::Predicate> BuildPredicate(const vector<DeltaMultiFileColumnDefinition> &columns,
                                               optional_ptr<const TableFilterSet> filters) {
	if (!filters || filters->filters.empty()) {
		return std::nullopt;
	}
	vector<delta::Predicate> predicates;
	for (auto &entry : filters->filters) {
		if (entry.first >= columns.size()) {
			return std::nullopt;
		}
		auto predicate = ToPredicate(columns[entry.first].name, *entry.second);
		if (!predicate) {
			return std::nullopt;
		}
		predicates.push_back(std::move(*predicate));
	}
	return delta::Predicate::all(std::move(predicates));
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
		if (std::getenv("DELTA_SCAN_IR_TRACE")) {
			fprintf(stderr, "[delta_scan_ir] reduce lowered plan: %s\n", ref->ToString().c_str());
		}
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
			if (!RunStatementToArrow(ctx, std::move(*shared_stmt), batch.mutable_array(), batch.mutable_schema())) {
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
	                            const delta::Predicate *predicate, ClientContext &context) {
	DuckDBEngine engine(context);
	auto snap_sm = delta::open_snapshot(path, version);
	delta::Snapshot snapshot = delta::drive(std::move(snap_sm), engine);
	delta::ScanKind sk = (kind == DeltaScanIRKind::Metadata) ? delta::ScanKind::Metadata : delta::ScanKind::Data;
	delta::Scan scan = snapshot.scan(engine, sk, predicate);
	const auto &result = scan.plan();
	DeltaPlanBuilder builder;
	auto terminal = builder.Lower(result);
	if (std::getenv("DELTA_SCAN_IR_TRACE")) {
		fprintf(stderr, "[delta_scan_ir] terminal lowered plan: %s\n", terminal->ToString().c_str());
	}
	return terminal;
}

} // namespace

unique_ptr<TableRef> BuildDeltaScanRef(const string &path, int64_t version, DeltaScanIRKind kind,
                                       const vector<DeltaMultiFileColumnDefinition> &columns,
                                       optional_ptr<const TableFilterSet> filters, ClientContext &context) {
	// Build the kernel data-skipping predicate from the pushed-down filters, here in the island (so no
	// ffi:: type crosses the facade). The kernel visits it ONCE synchronously inside the scan-SM open,
	// so the visitor need only outlive this call.
	auto predicate = BuildPredicate(columns, filters);

	// Optional diagnostics for verifying that the protobuf IR path served the scan.
	const bool trace = std::getenv("DELTA_SCAN_IR_TRACE") != nullptr;
	auto ref = BuildViaIR(path, version, kind, predicate ? &*predicate : nullptr, context);
	if (trace) {
		fprintf(stderr, "[delta_scan_ir] IR path served '%s' (kind=%s)\n", path.c_str(),
		        kind == DeltaScanIRKind::Metadata ? "metadata" : "data");
	}
	return ref;
}

} // namespace duckdb
