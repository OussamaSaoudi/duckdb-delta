//===----------------------------------------------------------------------===//
// DeltaPlanBuilder — proto plan-IR → DuckDB unbound TableRef (Phase D, D0 skeleton).
//
// D0 is the gate: the SSA-DAG walk + dispatch over the Operator oneof, with every NodeKind arm
// throwing `delta::DeltaError("unsupported node: ...")`. Coverage fills in one node per step
// Each supported kernel IR node lowers directly to DuckDB parser objects.
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_plan_builder.hpp"
#include "functions/delta_scan/delta_proto_lower.hpp" // SchemaToDuckDB, LowerPredicate

#include "delta_kernel.hpp" // delta::DeltaError

#include <atomic>

#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <string>

namespace duckdb {

using ::delta::kernel::plan::Operator;
using ::delta::kernel::plan::PlanNode;
using ::delta::kernel::plan::ResultPlan;
using ::delta::kernel::plan::ScanParquetNode;

namespace {

//! Wrap a SelectStatement in a SubqueryRef with a unique non-empty alias (see definition below for
//! why every emitted subquery must be aliased). Forward-declared so the leaf lowerings can use it.
unique_ptr<TableRef> MakeAliasedSubquery(unique_ptr<SelectStatement> stmt);

//! A process-unique, non-empty table alias with the given prefix. Every from-table a node emits must
//! carry one: DuckDB's `SELECT *` star-expansion qualifies each expanded column with the source's
//! binding alias, and an unset alias trips `BindingAlias::GetAlias on a non-set alias` (INTERNAL
//! Error). The SQL parser auto-aliases every table; hand-built TableRefs must set it explicitly.
inline std::string NextAlias(const char *prefix) {
	static std::atomic<uint64_t> counter {0};
	return std::string(prefix) + std::to_string(counter.fetch_add(1));
}

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

//! Convert a kernel FileMeta location (a URL string) to what read_parquet expects: a filesystem
//! path for `file://` URLs, or the URL verbatim for object-store schemes (s3://, etc.). Mirrors the
//! kernel URL normalization so local file lists use DuckDB-compatible paths.
std::string UrlToPath(const std::string &location) {
	const std::string file_scheme = "file://";
	if (location.rfind(file_scheme, 0) != 0) {
		return location; // non-file scheme: read_parquet handles the URL directly
	}
	// Strip "file://" and (best-effort) percent-decode %XX escapes back to raw bytes.
	std::string enc = location.substr(file_scheme.size());
	std::string out;
	out.reserve(enc.size());
	for (size_t i = 0; i < enc.size(); i++) {
		if (enc[i] == '%' && i + 2 < enc.size()) {
			auto hex = [](char c) -> int {
				if (c >= '0' && c <= '9') {
					return c - '0';
				}
				if (c >= 'a' && c <= 'f') {
					return c - 'a' + 10;
				}
				if (c >= 'A' && c <= 'F') {
					return c - 'A' + 10;
				}
				return -1;
			};
			int hi = hex(enc[i + 1]);
			int lo = hex(enc[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(enc[i]);
	}
	return out;
}

//! Build `read_parquet([<paths>], schema=<declared schema>)` as a TableFunctionRef.
//! The explicit schema is important for Delta checkpoints: action columns added by newer protocol
//! versions are legitimately absent from older checkpoint files, but the kernel plan declares them
//! and expects NULL values. DuckDB's Parquet schema option provides exactly those typed defaults.
unique_ptr<TableRef> ReadParquetRef(const ScanParquetNode &scan, const vector<string> &names,
                                    const vector<LogicalType> &types) {
	// The path list argument: list_value('p0', 'p1', ...).
	vector<unique_ptr<ParsedExpression>> paths;
	paths.reserve(scan.files_size());
	for (int i = 0; i < scan.files_size(); i++) {
		paths.push_back(make_uniq<ConstantExpression>(Value(UrlToPath(scan.files(i).location()))));
	}
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<FunctionExpression>("list_value", std::move(paths)));
	if (!names.empty()) {
		vector<Value> schema_keys;
		vector<Value> schema_values;
		schema_keys.reserve(names.size());
		schema_values.reserve(names.size());
		for (idx_t i = 0; i < names.size(); i++) {
			schema_keys.emplace_back(names[i]);
			schema_values.push_back(Value::STRUCT({{"name", Value(names[i])},
			                                       {"type", Value(types[i].ToString())},
			                                       {"default_value", Value(LogicalType::VARCHAR)}}));
		}
		// Capture this before moving schema_values. Passing schema_values[0].type() and
		// std::move(schema_values) as arguments to the same call is order-dependent in C++17.
		auto schema_value_type = schema_values[0].type();
		auto schema = make_uniq<ConstantExpression>(Value::MAP(LogicalType::VARCHAR, schema_value_type,
		                                                       std::move(schema_keys), std::move(schema_values)));
		schema->SetAlias("schema");
		args.push_back(std::move(schema));
	}

	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>("read_parquet", std::move(args));
	ref->alias = NextAlias("dkrp_");
	return ref;
}

//! Lower a ScanParquet node: a projection that CASTs every action column to its expected schema type
//! over read_parquet(...), so the row shape matches downstream regardless of the physical parquet
//! layout. Empty file list => a typed empty relation (SELECT NULL::t AS c, ... WHERE false).
unique_ptr<TableRef> LowerScanParquet(const ScanParquetNode &scan) {
	vector<string> names;
	vector<LogicalType> types;
	SchemaToDuckDB(scan.schema(), names, types);

	auto select = make_uniq<SelectNode>();
	const bool empty = scan.files_size() == 0;
	auto source = empty ? nullptr : ReadParquetRef(scan, names, types);
	const auto source_alias = source ? source->alias : string();
	for (size_t i = 0; i < names.size(); i++) {
		unique_ptr<ParsedExpression> col;
		if (empty) {
			// NULL cast to the column type — a typed placeholder for the empty relation.
			col = make_uniq<CastExpression>(types[i], make_uniq<ConstantExpression>(Value(types[i])));
		} else {
			// Qualify the input column. Without this, a projection such as
			// CAST(domainMetadata AS ...) AS domainMetadata is bound as a reference to its
			// own SELECT-list alias when reading a checkpoint with that column, which DuckDB
			// rejects as an alias self-reference.
			col = make_uniq<CastExpression>(types[i], make_uniq<ColumnRefExpression>(names[i], source_alias));
		}
		col->SetAlias(names[i]);
		select->select_list.push_back(std::move(col));
	}
	if (empty) {
		select->where_clause = make_uniq<ConstantExpression>(Value::BOOLEAN(false));
		select->from_table = make_uniq<EmptyTableRef>();
	} else {
		select->from_table = std::move(source);
	}

	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(select);
	return MakeAliasedSubquery(std::move(stmt));
}

//! Move out input `i` of a node, checking arity so an unexpected plan shape becomes a clean
//! DeltaError rather than an out-of-bounds crash.
unique_ptr<TableRef> TakeInput(const PlanNode &node, vector<unique_ptr<TableRef>> &inputs, size_t i) {
	if (i >= inputs.size() || !inputs[i]) {
		throw ::delta::DeltaError("DeltaPlanBuilder: node " + std::to_string(node.output()) + " expects input " +
		                          std::to_string(i) + " but has " + std::to_string(inputs.size()));
	}
	return std::move(inputs[i]);
}

//! Wrap a SelectStatement in a SubqueryRef with a UNIQUE non-empty alias. Every subquery a node emits
//! must be aliased: DuckDB's star expansion (`SELECT *` over the subquery) qualifies each expanded
//! column with the subquery's binding alias, and an unset alias trips
//! `BindingAlias::GetAlias on a non-set alias` (INTERNAL Error) when the plan nests subqueries. The
//! Hand-built TableRefs require explicit aliases for qualified column references.
//! not, so we stamp a unique alias here. Counter is per-process; only uniqueness + non-empty matter.
unique_ptr<TableRef> MakeAliasedSubquery(unique_ptr<SelectStatement> stmt) {
	static std::atomic<uint64_t> counter {0};
	auto sub = make_uniq<SubqueryRef>(std::move(stmt));
	sub->alias = "dksub_" + std::to_string(counter.fetch_add(1));
	return sub;
}

//! Wrap a SelectNode as a subquery TableRef (the uniform way a node hands its relation to its parent).
unique_ptr<TableRef> AsSubquery(unique_ptr<SelectNode> select) {
	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(select);
	return MakeAliasedSubquery(std::move(stmt));
}

//! Lower a Filter node: `SELECT * FROM <input> WHERE <predicate>`.
unique_ptr<TableRef> LowerFilter(const ::delta::kernel::plan::FilterNode &filter, unique_ptr<TableRef> input,
                                 const ::delta::kernel::schema::StructType *input_schema) {
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>());
	select->from_table = std::move(input);
	select->where_clause = LowerPredicate(filter.predicate(), input_schema);
	return AsSubquery(std::move(select));
}

//! Lower a Project node: `SELECT <expr> AS <name>, ... FROM <input>`. Each expression is lowered with
//! its output-schema field type (drives struct shaping) and the input relation schema (identity
//! Transforms rename physical→logical field names).
unique_ptr<TableRef> LowerProject(const ::delta::kernel::plan::ProjectNode &project, unique_ptr<TableRef> input,
                                  const ::delta::kernel::schema::StructType *input_schema) {
	const auto *out_schema = project.has_output_schema() ? &project.output_schema() : nullptr;
	auto select = make_uniq<SelectNode>();
	for (int i = 0; i < project.named_exprs_size(); i++) {
		const auto &ne = project.named_exprs(i);
		const ::delta::kernel::schema::DataType *expected =
		    (out_schema && i < out_schema->fields_size()) ? &out_schema->fields(i).data_type() : nullptr;
		auto expr = LowerExpr(ne.expr(), expected, input_schema);
		expr->SetAlias(ne.name());
		select->select_list.push_back(std::move(expr));
	}
	select->from_table = std::move(input);
	return AsSubquery(std::move(select));
}

//! Lower a Values node: a literal relation. `VALUES (r0c0, r0c1, ...), (...)` aliased to the schema
//! column names; empty rows => a typed empty relation (SELECT NULL::t AS c, ... WHERE false).
unique_ptr<TableRef> LowerValues(const ::delta::kernel::plan::ValuesNode &values) {
	vector<string> names;
	vector<LogicalType> types;
	SchemaToDuckDB(values.schema(), names, types);

	auto select = make_uniq<SelectNode>();
	if (values.rows_size() == 0) {
		for (size_t i = 0; i < names.size(); i++) {
			auto col = make_uniq<CastExpression>(types[i], make_uniq<ConstantExpression>(Value(types[i])));
			col->SetAlias(names[i]);
			select->select_list.push_back(std::move(col));
		}
		select->where_clause = make_uniq<ConstantExpression>(Value::BOOLEAN(false));
		select->from_table = make_uniq<EmptyTableRef>();
		return AsSubquery(std::move(select));
	}

	// Build an ExpressionListRef (VALUES) and give it the schema's column-name aliases PLUS a table
	// alias. The binder registers the VALUES relation's binding under `alias` (AddGenericBinding);
	// leaving it empty makes a downstream `SELECT *` expansion call GetAlias() on a non-set binding
	// alias -> `BindingAlias::GetAlias on a non-set alias` INTERNAL Error. Every from-table needs a
	// non-empty alias (the SQL parser auto-assigns one; hand-built refs must set it).
	static std::atomic<uint64_t> values_counter {0};
	auto rows_ref = make_uniq<ExpressionListRef>();
	rows_ref->alias = "dkvalues_" + std::to_string(values_counter.fetch_add(1));
	rows_ref->expected_types = types;
	rows_ref->expected_names = names;
	for (int r = 0; r < values.rows_size(); r++) {
		const auto &row = values.rows(r);
		vector<unique_ptr<ParsedExpression>> cells;
		for (int c = 0; c < row.values_size(); c++) {
			// Cast each literal to the column type so the VALUES relation carries the declared schema.
			size_t ci = static_cast<size_t>(c);
			LogicalType t = ci < types.size() ? types[ci] : LogicalType(LogicalType::SQLNULL);
			cells.push_back(make_uniq<CastExpression>(t, make_uniq<ConstantExpression>(ScalarToValue(row.values(c)))));
		}
		rows_ref->values.push_back(std::move(cells));
	}
	select->select_list.push_back(make_uniq<StarExpression>());
	select->from_table = std::move(rows_ref);
	return AsSubquery(std::move(select));
}

//! Build `read_json([<paths>], format='newline_delimited', columns={<name>: '<type>', ...})`.
unique_ptr<TableRef> ReadJsonRef(const ::delta::kernel::plan::ScanJsonNode &scan, const vector<string> &names,
                                 const vector<LogicalType> &types) {
	vector<unique_ptr<ParsedExpression>> paths;
	paths.reserve(scan.files_size());
	for (int i = 0; i < scan.files_size(); i++) {
		paths.push_back(make_uniq<ConstantExpression>(Value(UrlToPath(scan.files(i).location()))));
	}
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<FunctionExpression>("list_value", std::move(paths)));

	auto fmt = make_uniq<ConstantExpression>(Value("newline_delimited"));
	fmt->SetAlias("format");
	args.push_back(std::move(fmt));

	// columns={'name': 'TYPE', ...}: a struct whose values are DuckDB type-name strings, matching the
	// kernel's read_json rendering (each column forced to its schema type).
	vector<unique_ptr<ParsedExpression>> cols;
	for (size_t i = 0; i < names.size(); i++) {
		auto type_str = make_uniq<ConstantExpression>(Value(types[i].ToString()));
		type_str->SetAlias(names[i]);
		cols.push_back(std::move(type_str));
	}
	auto columns = make_uniq<FunctionExpression>("struct_pack", std::move(cols));
	columns->SetAlias("columns");
	args.push_back(std::move(columns));

	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>("read_json", std::move(args));
	ref->alias = NextAlias("dkrj_");
	return ref;
}

//! Lower a ScanJson node: `SELECT <cols> FROM read_json(...)` (columns already forced by read_json's
//! `columns` param). Empty file list => a typed empty relation.
unique_ptr<TableRef> LowerScanJson(const ::delta::kernel::plan::ScanJsonNode &scan) {
	vector<string> names;
	vector<LogicalType> types;
	SchemaToDuckDB(scan.schema(), names, types);

	auto select = make_uniq<SelectNode>();
	if (scan.files_size() == 0) {
		for (size_t i = 0; i < names.size(); i++) {
			auto col = make_uniq<CastExpression>(types[i], make_uniq<ConstantExpression>(Value(types[i])));
			col->SetAlias(names[i]);
			select->select_list.push_back(std::move(col));
		}
		select->where_clause = make_uniq<ConstantExpression>(Value::BOOLEAN(false));
		select->from_table = make_uniq<EmptyTableRef>();
		return AsSubquery(std::move(select));
	}
	for (const auto &name : names) {
		select->select_list.push_back(make_uniq<ColumnRefExpression>(name));
	}
	select->from_table = ReadJsonRef(scan, names, types);
	return AsSubquery(std::move(select));
}

//! Lower a join key expression, qualifying a bare column reference with the join side's alias
//! (l_side/r_side) so the ON condition is unambiguous. Non-column keys lower generically (the alias
//! is resolvable through the aliased subquery side).
unique_ptr<ParsedExpression> QualifyKey(const ::delta::kernel::expressions::Expression &key, const string &side) {
	if (key.kind_case() == ::delta::kernel::expressions::Expression::kColumn) {
		vector<string> path;
		path.push_back(side);
		for (const auto &part : key.column().path()) {
			path.push_back(part);
		}
		return make_uniq<ColumnRefExpression>(std::move(path));
	}
	return LowerExpr(key);
}

//! Extract the single top-level name of a kernel ColumnName, or throw (delta_load requires the
//! path/size/dv/derived columns to be top-level input columns).
std::string TopLevelName(const ::delta::kernel::expressions::ColumnName &col, const char *what) {
	if (col.path_size() != 1) {
		throw ::delta::DeltaError(std::string("DeltaPlanBuilder: ") + what + " must be a top-level column");
	}
	return col.path(0);
}

//! Collect Delta column-mapping field ids (`delta.columnMapping.id`) in DFS pre-order over a struct:
//! each field's id (or a NULL placeholder), then recurse into struct-typed children. Order matches
//! the kernel operator's recursive column rename, so ids line up with top-level and nested columns.
//! Returns false into `any` untouched; sets `any=true` if at least one real id was found.
void CollectFieldIdsDfs(const ::delta::kernel::schema::StructType &st,
                        vector<unique_ptr<ParsedExpression>> &out, bool &any) {
	for (const auto &f : st.fields()) {
		auto it = f.metadata().find("delta.columnMapping.id");
		if (it != f.metadata().end() && it->second.value_case() == ::delta::kernel::schema::MetadataValue::kNumber) {
			out.push_back(make_uniq<ConstantExpression>(Value::BIGINT(it->second.number())));
			any = true;
		} else {
			out.push_back(make_uniq<ConstantExpression>(Value(LogicalType::BIGINT)));
		}
		if (f.data_type().kind_case() == ::delta::kernel::schema::DataType::kStruct) {
			CollectFieldIdsDfs(f.data_type().struct_(), out, any);
		}
	}
}

//! Lower a Load node to the `delta_load` table function over its input relation — the faithful,
//! generic realization of the kernel Load IR node (opens each file, applies the per-row deletion
//! vector, broadcasts metadata-derived columns). The input relation is passed as a table-valued
//! subquery argument, projected to just the columns delta_load reads.
unique_ptr<TableRef> LowerLoad(const ::delta::kernel::plan::LoadNode &load, unique_ptr<TableRef> input) {
	using ::delta::kernel::plan::FileType;
	const std::string path_col = TopLevelName(load.file_meta().path_column(), "delta_load path_column");

	// Project the input to exactly the columns delta_load reads (path, size, rowcount, dv, derived),
	// so nothing downstream forces reading more and DuckDB can push the projection toward the read.
	vector<string> input_cols;
	auto push = [&](const std::string &c) {
		for (const auto &e : input_cols) {
			if (e == c) {
				return;
			}
		}
		input_cols.push_back(c);
	};
	push(path_col);
	if (load.file_meta().has_file_size_column()) {
		push(TopLevelName(load.file_meta().file_size_column(), "delta_load file_size_column"));
	}
	if (load.file_meta().has_num_records_column()) {
		push(TopLevelName(load.file_meta().num_records_column(), "delta_load num_records_column"));
	}
	if (load.has_dv_ref()) {
		push(TopLevelName(load.dv_ref().column(), "delta_load dv_column"));
	}
	for (int i = 0; i < load.metadata_derived_columns_size(); i++) {
		push(TopLevelName(load.metadata_derived_columns(i), "delta_load metadata_derived"));
	}

	auto proj = make_uniq<SelectNode>();
	for (const auto &c : input_cols) {
		proj->select_list.push_back(make_uniq<ColumnRefExpression>(c));
	}
	proj->from_table = std::move(input);
	auto proj_stmt = make_uniq<SelectStatement>();
	proj_stmt->node = std::move(proj);
	auto input_subquery = make_uniq<SubqueryExpression>();
	input_subquery->subquery_type = SubqueryType::SCALAR;
	input_subquery->subquery = std::move(proj_stmt);

	// file_schema rendered as a DuckDB STRUCT type string (physical read columns/types).
	vector<string> fnames;
	vector<LogicalType> ftypes;
	SchemaToDuckDB(load.file_schema(), fnames, ftypes);
	child_list_t<LogicalType> struct_children;
	for (size_t i = 0; i < fnames.size(); i++) {
		struct_children.emplace_back(fnames[i], ftypes[i]);
	}
	const std::string file_schema_str = LogicalType::STRUCT(std::move(struct_children)).ToString();
	const char *file_type = load.file_type() == FileType::FILE_TYPE_JSON ? "json" : "parquet";

	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(input_subquery));
	auto named = [&](const string &name, Value v) {
		auto e = make_uniq<ConstantExpression>(std::move(v));
		e->SetAlias(name);
		args.push_back(std::move(e));
	};
	named("file_type", Value(std::string(file_type)));
	named("file_schema", Value(file_schema_str));
	named("path_column", Value(path_col));
	if (load.has_base_url()) {
		named("base_url", Value(load.base_url()));
	}
	if (load.has_dv_ref()) {
		named("dv_column", Value(TopLevelName(load.dv_ref().column(), "delta_load dv_column")));
		const char *kind = load.dv_ref().kind() == ::delta::kernel::plan::DV_KIND_BYTES ? "bytes" : "descriptor";
		named("dv_kind", Value(std::string(kind)));
	}
	if (load.metadata_derived_columns_size() > 0) {
		vector<unique_ptr<ParsedExpression>> derived;
		for (int i = 0; i < load.metadata_derived_columns_size(); i++) {
			derived.push_back(make_uniq<ConstantExpression>(
			    Value(TopLevelName(load.metadata_derived_columns(i), "delta_load metadata_derived"))));
		}
		auto lst = make_uniq<FunctionExpression>("list_value", std::move(derived));
		lst->SetAlias("metadata_derived");
		args.push_back(std::move(lst));
	}
	// Column-mapping field ids (top-level AND nested, DFS pre-order) so the read maps by field id.
	vector<unique_ptr<ParsedExpression>> ids;
	bool any_ids = false;
	CollectFieldIdsDfs(load.file_schema(), ids, any_ids);
	if (any_ids) {
		auto lst = make_uniq<FunctionExpression>("list_value", std::move(ids));
		lst->SetAlias("field_ids");
		args.push_back(std::move(lst));
	}

	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>("delta_load", std::move(args));
	ref->alias = NextAlias("dkdl_");
	return ref;
}

//! Lower a MaxByVersion node: keep the latest row per group via `arg_max(struct_pack(cols), version)`,
//! then unpack. Matches the kernel's streaming hash-aggregate (no window/sort).
unique_ptr<TableRef> LowerMaxByVersion(const ::delta::kernel::plan::MaxByVersionNode &mbv,
                                       unique_ptr<TableRef> input,
                                       const ::delta::kernel::schema::StructType *input_schema) {
	vector<string> names;
	vector<LogicalType> types;
	SchemaToDuckDB(mbv.output_schema(), names, types);

	// Inner: SELECT arg_max(struct_pack(c := c, ...), <version>) AS __mbv FROM <input> GROUP BY <group_by>
	auto inner = make_uniq<SelectNode>();
	vector<unique_ptr<ParsedExpression>> packs;
	for (const auto &name : names) {
		auto v = make_uniq<ColumnRefExpression>(name);
		v->SetAlias(name); // struct_pack(name := name)
		packs.push_back(std::move(v));
	}
	auto packed = make_uniq<FunctionExpression>("struct_pack", std::move(packs));
	vector<unique_ptr<ParsedExpression>> am_args;
	am_args.push_back(std::move(packed));
	am_args.push_back(LowerExpr(mbv.version_column(), nullptr, input_schema));
	auto arg_max = make_uniq<FunctionExpression>("arg_max", std::move(am_args));
	arg_max->SetAlias("__mbv");
	inner->select_list.push_back(std::move(arg_max));
	inner->from_table = std::move(input);
	for (int i = 0; i < mbv.group_by_size(); i++) {
		inner->groups.group_expressions.push_back(LowerExpr(mbv.group_by(i), nullptr, input_schema));
		inner->groups.grouping_sets.resize(1);
		inner->groups.grouping_sets[0].insert(static_cast<idx_t>(i));
	}

	// Outer: SELECT __mbv.c AS c, ... FROM (inner)
	auto outer = make_uniq<SelectNode>();
	for (const auto &name : names) {
		auto access = make_uniq<ColumnRefExpression>(name, "__mbv");
		access->SetAlias(name);
		outer->select_list.push_back(std::move(access));
	}
	outer->from_table = AsSubquery(std::move(inner));
	return AsSubquery(std::move(outer));
}

//! Give a lowered input an alias and wrap it so it can be a named join side. TableRef carries the
//! alias, but a SubqueryRef is where we can set it uniformly; wrap non-subquery refs in `SELECT *`.
unique_ptr<TableRef> AliasedSide(unique_ptr<TableRef> input, const string &alias) {
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>());
	select->from_table = std::move(input);
	auto sub = AsSubquery(std::move(select));
	sub->alias = alias;
	return sub;
}

//! Lower an EquiJoin (currently only LeftAnti): `SELECT l_side.* FROM <left> l_side ANTI JOIN
//! <right> r_side ON (l_side.<lkey> IS NOT DISTINCT FROM r_side.<rkey>) AND ...`.
unique_ptr<TableRef> LowerEquiJoin(const ::delta::kernel::plan::EquiJoinNode &join, unique_ptr<TableRef> left,
                                   unique_ptr<TableRef> right) {
	using ::delta::kernel::plan::JoinKind;
	if (join.kind() != ::delta::kernel::plan::JOIN_KIND_LEFT_ANTI) {
		throw ::delta::DeltaError("DeltaPlanBuilder: unsupported EquiJoin kind");
	}
	if (join.left_keys_size() != join.right_keys_size() || join.left_keys_size() == 0) {
		throw ::delta::DeltaError("DeltaPlanBuilder: EquiJoin key arity mismatch");
	}

	auto join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join_ref->type = JoinType::ANTI;
	join_ref->left = AliasedSide(std::move(left), "l_side");
	join_ref->right = AliasedSide(std::move(right), "r_side");

	// Build the ON condition: AND of (l_side.<lkey> IS NOT DISTINCT FROM r_side.<rkey>). The keys are
	// expressions over each side; qualify a bare column key with the side alias.
	unique_ptr<ParsedExpression> cond;
	for (int i = 0; i < join.left_keys_size(); i++) {
		auto l = QualifyKey(join.left_keys(i), "l_side");
		auto r = QualifyKey(join.right_keys(i), "r_side");
		auto cmp = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM, std::move(l),
		                                           std::move(r));
		if (!cond) {
			cond = std::move(cmp);
		} else {
			cond = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(cond),
			                                        std::move(cmp));
		}
	}
	join_ref->condition = std::move(cond);

	// SELECT l_side.* FROM (<left> l_side ANTI JOIN <right> r_side ON ...)
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>("l_side"));
	select->from_table = std::move(join_ref);
	return AsSubquery(std::move(select));
}

//! Lower a UnionAll: `SELECT * FROM in0 UNION ALL BY NAME SELECT * FROM in1 ...` as a single N-ary
//! SetOperationNode (UNION_BY_NAME, ALL). Matches the kernel's `UNION ALL BY NAME`.
unique_ptr<TableRef> LowerUnionAll(vector<unique_ptr<TableRef>> inputs) {
	if (inputs.empty()) {
		throw ::delta::DeltaError("DeltaPlanBuilder: UnionAll with no inputs");
	}
	auto make_child = [](unique_ptr<TableRef> in) -> unique_ptr<QueryNode> {
		auto s = make_uniq<SelectNode>();
		s->select_list.push_back(make_uniq<StarExpression>());
		s->from_table = std::move(in);
		return std::move(s);
	};
	if (inputs.size() == 1) {
		auto stmt = make_uniq<SelectStatement>();
		stmt->node = make_child(std::move(inputs[0]));
		return MakeAliasedSubquery(std::move(stmt));
	}
	auto setop = make_uniq<SetOperationNode>();
	setop->setop_type = SetOperationType::UNION_BY_NAME;
	setop->setop_all = true;
	for (auto &in : inputs) {
		setop->children.push_back(make_child(std::move(in)));
	}
	auto stmt = make_uniq<SelectStatement>();
	stmt->node = std::move(setop);
	return MakeAliasedSubquery(std::move(stmt));
}

//! Compute the actual relation schema produced by Load: physical file columns followed by every
//! metadata-derived column copied from the descriptor input. The proto stores only file_schema, so
//! the engine must synthesize this shape before lowering the terminal Project/Transform.
unique_ptr<::delta::kernel::schema::StructType> LoadOutputSchema(
    const ::delta::kernel::plan::LoadNode &load,
    const ::delta::kernel::schema::StructType *input_schema) {
	auto output = make_uniq<::delta::kernel::schema::StructType>();
	output->CopyFrom(load.file_schema());
	if (load.metadata_derived_columns_size() == 0) {
		return output;
	}
	if (!input_schema) {
		throw ::delta::DeltaError("DeltaPlanBuilder: Load metadata-derived columns require an input schema");
	}
	for (const auto &column : load.metadata_derived_columns()) {
		const std::string name = TopLevelName(column, "delta_load metadata_derived");
		const ::delta::kernel::schema::StructField *found = nullptr;
		for (const auto &field : input_schema->fields()) {
			if (field.name() == name) {
				found = &field;
				break;
			}
		}
		if (!found) {
			throw ::delta::DeltaError("DeltaPlanBuilder: Load metadata-derived column '" + name +
			                          "' is absent from its input schema");
		}
		output->add_fields()->CopyFrom(*found);
	}
	return output;
}

} // namespace

const ::delta::kernel::schema::StructType *DeltaPlanBuilder::NodeOutputSchema(const PlanNode &node) const {
	const Operator &op = node.op();
	switch (op.op_case()) {
	case Operator::kProject:
		return op.project().has_output_schema() ? &op.project().output_schema() : nullptr;
	case Operator::kMaxByVersion:
		return op.max_by_version().has_output_schema() ? &op.max_by_version().output_schema() : nullptr;
	case Operator::kLoad:
		// Synthesized during Lower(); see LoadOutputSchema.
		return nullptr;
	case Operator::kValues:
		return op.values().has_schema() ? &op.values().schema() : nullptr;
	case Operator::kScanParquet:
		return op.scan_parquet().has_schema() ? &op.scan_parquet().schema() : nullptr;
	case Operator::kScanJson:
		return op.scan_json().has_schema() ? &op.scan_json().schema() : nullptr;
	case Operator::kFilter: {
		// Filter passes its input's schema through.
		if (node.inputs_size() == 0) {
			return nullptr;
		}
		auto it = schemas_.find(node.inputs(0));
		return it == schemas_.end() ? nullptr : it->second;
	}
	default:
		return nullptr;
	}
}

unique_ptr<TableRef> DeltaPlanBuilder::Lower(const ResultPlan &result_plan) {
	lowered_.clear();
	schemas_.clear();
	owned_schemas_.clear();
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
			// The IR is a DAG, not a tree: a node can be a shared input to more than one parent
			// (e.g. the scan reconciliation feeds `commit_dedup` into both a left-anti-join and the
			// final union). Copy the child subtree for each consumer and keep the original in the map,
			// so a later consumer of the same RefId still finds it. (The kernel's SQL lowering avoids
			// duplication by emitting one CTE per node; representing the shared subtree as a CTE here
			// instead of copying it is a possible future optimization — copying is correct, since the
			// IR is deterministic dataflow, just potentially redundant for a fanned-out subtree.)
			inputs.push_back(it->second->Copy());
		}
		// The single input relation's schema (if tracked), threaded into expression lowering so a
		// Transform/Project over it can rename physical→logical field names.
		const ::delta::kernel::schema::StructType *input_schema = nullptr;
		if (node.inputs_size() > 0) {
			auto sit = schemas_.find(node.inputs(0));
			input_schema = sit == schemas_.end() ? nullptr : sit->second;
		}
		lowered_[node.output()] = LowerNode(node, std::move(inputs), input_schema);
		if (node.op().op_case() == Operator::kLoad) {
			auto schema = LoadOutputSchema(node.op().load(), input_schema);
			schemas_[node.output()] = schema.get();
			owned_schemas_[node.output()] = std::move(schema);
		} else {
			schemas_[node.output()] = NodeOutputSchema(node);
		}
	}

	uint32_t result_ref = result_plan.result();
	auto it = lowered_.find(result_ref);
	if (it == lowered_.end()) {
		throw ::delta::DeltaError("DeltaPlanBuilder: terminal RefId " + std::to_string(result_ref) +
		                          " was not produced by the plan");
	}
	return std::move(it->second);
}

unique_ptr<TableRef> DeltaPlanBuilder::LowerNode(const PlanNode &node, vector<unique_ptr<TableRef>> inputs,
                                                 const ::delta::kernel::schema::StructType *input_schema) {
	const Operator &op = node.op();
	switch (op.op_case()) {
	case Operator::kScanParquet:
		return LowerScanParquet(op.scan_parquet());
	case Operator::kValues:
		return LowerValues(op.values());
	case Operator::kFilter:
		return LowerFilter(op.filter(), TakeInput(node, inputs, 0), input_schema);
	case Operator::kProject:
		return LowerProject(op.project(), TakeInput(node, inputs, 0), input_schema);
	case Operator::kScanJson:
		return LowerScanJson(op.scan_json());
	case Operator::kLoad:
		return LowerLoad(op.load(), TakeInput(node, inputs, 0));
	case Operator::kMaxByVersion:
		return LowerMaxByVersion(op.max_by_version(), TakeInput(node, inputs, 0), input_schema);
	case Operator::kEquiJoin:
		return LowerEquiJoin(op.equi_join(), TakeInput(node, inputs, 0), TakeInput(node, inputs, 1));
	case Operator::kUnionAll:
		return LowerUnionAll(std::move(inputs));
	// ListFiles is a metadata-listing node with no data-plane lowering (resolved by the SM before the
	// result plan is emitted); it should not appear in a ResultPlan. D7 leaves it throwing.
	case Operator::kListFiles:
	case Operator::OP_NOT_SET:
	default:
		throw ::delta::DeltaError(std::string("DeltaPlanBuilder: unsupported node: ") +
		                          OpCaseName(op.op_case()));
	}
}

} // namespace duckdb
