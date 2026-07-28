#include "functions/delta_scan/physical_delta_scan.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include <cstdlib>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Operator-path filter pushdown (optimizer extension).
//
// The custom LogicalDeltaGet (a LogicalExtensionOperator) is invisible to DuckDB's FilterPushdown,
// which only knows LogicalGet — so the pushed-down WHERE clause lands as a LogicalFilter ABOVE the
// operator and the operator's DeltaMultiFileList keeps EMPTY table_filters (no data-skipping).
//
// This extension runs after DuckDB's optimizers: it finds each LogicalFilter directly over a
// LogicalDeltaGet (delta_scan only), converts those filter expressions into a TableFilterSet with the
// same FilterCombiner the standard path uses, pushes them into the operator's DeltaMultiFileList
// (populating table_filters), and swaps the resulting filtered list into the operator's bind_data. The
// reconciliation subplan then builds its PredicateVisitor from those table_filters,
// so it prunes files exactly like step-1. The LogicalFilter is left in place (it re-checks rows; the
// pushdown only adds stats-based file skipping — never changes results). Only active for the operator path.
//===--------------------------------------------------------------------===//
static void DeltaScanPushdownIntoOperator(LogicalDeltaGet &op, vector<unique_ptr<Expression>> &filter_exprs,
                                          ClientContext &context) {
	// delta_load (faithful Load) sets build columns; its file list is sink-fed, not snapshot-driven, so
	// there is nothing to push a stats predicate into. Only plain delta_scan (no build child) applies.
	if (op.build_path_col != DConstants::INVALID_INDEX || !op.bind_data) {
		return;
	}
	auto *mf_bind = dynamic_cast<MultiFileBindData *>(op.bind_data.get());
	if (!mf_bind || !mf_bind->file_list) {
		return;
	}
	auto *delta_list = dynamic_cast<DeltaMultiFileList *>(mf_bind->file_list.get());
	if (!delta_list) {
		return;
	}

	// Build a MultiFilePushdownInfo for a full scan: column_ids = 0..n-1, column_indexes likewise. The
	// filter expressions reference ColumnBinding(op.bind_index, col), matching GenerateColumnBindings.
	idx_t n = op.returned_types.size();
	vector<ColumnIndex> column_indexes;
	column_indexes.reserve(n);
	for (idx_t i = 0; i < n; i++) {
		column_indexes.emplace_back(i);
	}

	// Convert the filter expressions into a TableFilterSet via the same FilterCombiner the standard path
	// uses (multi_file ComplexFilterPushdown -> DeltaMultiFileList::ComplexFilterPushdown). We call the
	// lower-level PushdownInternal directly (NOT ComplexFilterPushdown) on purpose: ComplexFilterPushdown
	// runs ReportFilterPushdown, which — when delta_scan_explain_files_filtered is on — calls
	// GetTotalFileCount on the new list and EAGERLY populates it via the synchronous kernel iterator. The
	// reconciliation subplan's build sink would then append a SECOND time, double-counting files. Going
	// through PushdownInternal keeps the new list empty so the subplan sink is its sole populator.
	FilterCombiner combiner(context);
	for (auto riter = filter_exprs.rbegin(); riter != filter_exprs.rend(); ++riter) {
		combiner.AddFilter((*riter)->Copy());
	}
	vector<FilterPushdownResult> pushdown_results;
	auto filter_set = combiner.GenerateTableScanFilters(column_indexes, pushdown_results);
	if (filter_set.filters.empty()) {
		return; // nothing prunable
	}
	auto new_list = delta_list->PushdownInternal(context, filter_set);
	// The reconciliation subplan feeds the swapped-in list from a build sink. Mark it build-populated so a
	// bind/plan-time GetCardinality on this list serves resolved_files (empty) instead of triggering the
	// synchronous kernel scan. Mirrors the bind-time mark in DeltaScanBindOperator.
	new_list->MarkBuildPopulated();
	mf_bind->file_list = shared_ptr<MultiFileList>(std::move(new_list));
}

//===--------------------------------------------------------------------===//
// Reconciliation subplan attachment. Turn the delta_scan operator's metadata reconciliation
// into a REAL bound child subplan so EXPLAIN shows its nodes (READ_JSON per commit, UNION, arg_max dedup,
// tombstone FILTER, and — when a WHERE predicate was pushed down — the data-skipping FILTER over
// add.stats_parsed). Runs post-pushdown, so the dynamic predicate already threaded into the operator's
// DeltaMultiFileList::table_filters is baked into the kernel-lowered plan. PhysicalDeltaLoad's existing
// build sink then populates the file list from this subplan's rows (path / fileConstantValues /
// deletionVector), exactly as the delta_load TVF does — no nested Connection.
//===--------------------------------------------------------------------===//
void BindDeltaReconciliationChild(LogicalDeltaGet &op, ClientContext &context, Binder *parent_binder) {
	if (op.is_load || !op.bind_data) {
		return;
	}
	auto *mf_bind = dynamic_cast<MultiFileBindData *>(op.bind_data.get());
	if (!mf_bind || !mf_bind->file_list) {
		return;
	}
	auto *delta_list = dynamic_cast<DeltaMultiFileList *>(mf_bind->file_list.get());
	if (!delta_list) {
		return;
	}

	// Lower the kernel metadata-only scan SM to a reconciliation TableRef via the proto-IR path
	// (DeltaPlanBuilder), WITH the pushed-down predicate baked in (the stats-based file-skip FILTER over
	// add.stats_parsed is present exactly when a WHERE predicate reached table_filters). DuckDB drives
	// the SM (executes each Reduce) inside the protobuf-only facade.
	auto recon_ref = delta_list->BuildReconciliationRef(context);

	// Bind the TableRef into a fully-planned child LogicalOperator. Wrap it as `SELECT * FROM (<ref>)`
	// so it binds as a query. The child binder shares the query's GlobalBinderState (bound_tables
	// counter) with the main plan's binder, so it allocates FRESH table indices past every main-plan
	// index — no binding collisions. Done post-pushdown so the predicate is known.
	auto recon_select = make_uniq<SelectNode>();
	recon_select->select_list.push_back(make_uniq<StarExpression>());
	recon_select->from_table = std::move(recon_ref);
	auto recon_stmt = make_uniq<SelectStatement>();
	recon_stmt->node = std::move(recon_select);
	auto child_binder = Binder::CreateBinder(context, parent_binder);
	// Bind through the public SQLStatement& overload (Bind(SelectStatement&) is private).
	SQLStatement &recon_sql_stmt = *recon_stmt;
	auto bound = child_binder->Bind(recon_sql_stmt);
	if (!bound.plan) {
		throw IOException("delta_scan: failed to bind reconciliation subplan");
	}

	// The metadata-only terminal emits columns path / fileConstantValues / deletionVector (+ size). Map
	// their positions in the subplan's output so PhysicalDeltaLoad's sink ingests each row.
	for (idx_t c = 0; c < bound.names.size(); c++) {
		const auto &nm = bound.names[c];
		if (nm == "path") {
			op.build_path_col = c;
		} else if (nm == "fileConstantValues") {
			op.build_fcv_col = c;
		} else if (nm == "deletionVector") {
			op.build_dv_col = c;
		}
	}
	if (op.build_path_col == DConstants::INVALID_INDEX) {
		throw IOException("delta_scan: reconciliation subplan is missing the 'path' column");
	}

	// Attach as the operator's build child. CreatePlan plans it as the build/sink pipeline; BuildPipelines
	// runs it as a barriered child meta pipeline feeding the source. Attached AFTER all optimizers, so
	// RemoveUnusedColumns never prunes the subplan's terminal columns (the sink reads them by position).
	op.children.clear();
	op.children.push_back(std::move(bound.plan));
	// The optimizer may replace the bind-time child after ResolveTypes has already recorded column
	// dependencies. Rebuild them against the replacement's fresh table indexes; retaining the old
	// BoundColumnRefExpressions makes the physical planner look for bindings that no longer exist.
	op.expressions.clear();
	op.children[0]->ResolveOperatorTypes();
	auto child_bindings = op.children[0]->GetColumnBindings();
	for (idx_t i = 0; i < child_bindings.size(); i++) {
		op.expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(op.children[0]->types[i], child_bindings[i]));
	}
}

static void DeltaScanAttachReconciliationSubplan(LogicalDeltaGet &op, OptimizerExtensionInput &input) {
	// Replace the bind-time baseline with a plan built from the post-pushdown file list. delta_load
	// already carries its Load input child and must never pass through this path.
	BindDeltaReconciliationChild(op, input.context, &input.optimizer.binder);
}

static void DeltaScanOptimizeFilterPushdown(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	// Walk the plan: for each LogicalFilter directly over a delta LogicalDeltaGet, push its predicates
	// into the operator's file list (populating table_filters — the dynamic data-skipping predicate).
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER && !plan->children.empty() &&
	    plan->children[0]->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = plan->children[0]->Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "delta_scan") {
			DeltaScanPushdownIntoOperator(ext.Cast<LogicalDeltaGet>(), plan->expressions, input.context);
		}
	}
	// Attach the reconciliation subplan to EVERY delta_scan operator (with or without a WHERE above it).
	// Done when we directly reach the operator in the walk — after the parent filter (if any) has already
	// pushed its predicate into the file list above — so the subplan SQL reflects it. Every delta_scan's
	// file list was marked build-populated (at bind / on pushdown), so it MUST get a sink: attaching the
	// subplan unconditionally is what populates it.
	if (plan->type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		auto &ext = plan->Cast<LogicalExtensionOperator>();
		if (ext.GetExtensionName() == "delta_scan") {
			DeltaScanAttachReconciliationSubplan(ext.Cast<LogicalDeltaGet>(), input);
		}
	}
	for (auto &child : plan->children) {
		DeltaScanOptimizeFilterPushdown(input, child);
	}
}

void RegisterDeltaScanOptimizer(DBConfig &config) {
	OptimizerExtension ext;
	ext.optimize_function = DeltaScanOptimizeFilterPushdown;
	OptimizerExtension::Register(config, ext);
}

PhysicalOperator &LogicalDeltaGet::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	if (column_ids.empty()) {
		auto empty = virtual_columns.find(COLUMN_IDENTIFIER_EMPTY);
		column_ids.emplace_back(empty != virtual_columns.end() ? COLUMN_IDENTIFIER_EMPTY : 0);
		ResolveTypes();
	}

	vector<LogicalType> output_types = types;
	vector<LogicalType> all_types = returned_types;
	vector<string> names = returned_names;
	auto params = parameters;
	auto vcols = virtual_columns;

	auto &scan = planner.Make<PhysicalDeltaLoad>(
	    std::move(output_types), function, std::move(bind_data), std::move(all_types), std::move(column_ids),
	    vector<idx_t>(), std::move(names), unique_ptr<TableFilterSet>(), estimated_cardinality, ExtraOperatorInfo(),
	    std::move(params), std::move(vcols));

	// M1: if a metadata-reconciliation subplan was attached at bind time, plan it as the build child
	// and forward the scan_file_row column positions so the sink can ingest them.
	if (!children.empty()) {
		auto &child_plan = planner.CreatePlan(*children[0]);
		scan.children.push_back(child_plan);
		auto &delta_scan = scan.Cast<PhysicalDeltaLoad>();
		delta_scan.build_path_col = build_path_col;
		delta_scan.build_fcv_col = build_fcv_col;
		delta_scan.build_dv_col = build_dv_col;
		delta_scan.build_metadata_cols = build_metadata_cols;
	}
	return scan;
}

// Reach the DeltaMultiFileList that the source side reads, via the operator's bind data.
static DeltaMultiFileList &GetDeltaFileList(FunctionData &bind_data) {
	auto &mf_bind_data = bind_data.Cast<MultiFileBindData>();
	return mf_bind_data.file_list->Cast<DeltaMultiFileList>();
}

//===--------------------------------------------------------------------===//
// Build sink (M1.1): consume the metadata-reconciliation subplan's scan_file_rows and build the
// surviving file list directly into the DeltaMultiFileList that the source side reads. This is the
// faithful realization of the kernel data-stage Load's input: the build pipeline produces the
// scan_file_rows; the sink ingests each (path, fileConstantValues, deletionVector) row.
//===--------------------------------------------------------------------===//
class DeltaScanBuildGlobalState : public GlobalSinkState {};

// Local sink state. Each Sink() chunk publishes its resolved entries to the shared list; the source
// starts only after the child pipeline finalizes. DV resolution remains lock-free in BuildFileEntry
// before the short locked append.
class DeltaScanBuildLocalState : public LocalSinkState {
public:
	vector<DeltaMultiFileList::ResolvedFileEntry> entries;
};

unique_ptr<GlobalSinkState> PhysicalDeltaLoad::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<DeltaScanBuildGlobalState>();
}

unique_ptr<LocalSinkState> PhysicalDeltaLoad::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<DeltaScanBuildLocalState>();
}

SinkResultType PhysicalDeltaLoad::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<DeltaScanBuildLocalState>();
	auto &file_list = GetDeltaFileList(*bind_data);
	chunk.Flatten();
	if (std::getenv("DELTA_SCAN_IR_TRACE")) {
		string cols;
		for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
			cols += (c ? " | " : "") + std::to_string(c) + "=" +
			        (chunk.size() ? chunk.GetValue(c, 0).ToString() : string("<empty>"));
		}
		fprintf(stderr, "[delta_load Sink] build_path_col=%llu chunk.cols=%llu size=%llu row0=[%s]\n",
		        (unsigned long long)build_path_col, (unsigned long long)chunk.ColumnCount(),
		        (unsigned long long)chunk.size(), cols.c_str());
	}
	lstate.entries.clear();
	for (idx_t r = 0; r < chunk.size(); r++) {
		Value path_val = chunk.GetValue(build_path_col, r);
		Value fcv_val = build_fcv_col != DConstants::INVALID_INDEX ? chunk.GetValue(build_fcv_col, r) : Value();
		Value dv_val = build_dv_col != DConstants::INVALID_INDEX ? chunk.GetValue(build_dv_col, r) : Value();
		vector<pair<string, Value>> metadata_values;
		metadata_values.reserve(build_metadata_cols.size());
		for (const auto &metadata_col : build_metadata_cols) {
			metadata_values.emplace_back(metadata_col.first, chunk.GetValue(metadata_col.second, r));
		}
		// Lock-free build (resolves the DV in parallel across sink threads).
		auto entry = file_list.BuildFileEntry(path_val, fcv_val, dv_val, metadata_values);
		if (!entry.file.path.empty()) {
			lstate.entries.push_back(std::move(entry));
		}
	}
	// Publish this chunk's files to the shared build list under one lock.
	file_list.AppendResolvedEntries(std::move(lstate.entries));
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalDeltaLoad::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	// Nothing to flush — Sink publishes each chunk immediately. (Kept for the sink API.)
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalDeltaLoad::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                             OperatorSinkFinalizeInput &input) const {
	// The input feed is fully consumed: close the listing (no more files) and wake any source blocked
	// at the tail so it observes closed-and-drained and finishes.
	GetDeltaFileList(*bind_data).MarkExternallyPopulated();
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Pipeline construction: build (metadata subplan) -> sink(this); probe (data scan) = source(this).
// Mirrors PhysicalCTE / the join-build pattern. Falls back to a plain source when there is no child.
//===--------------------------------------------------------------------===//
void PhysicalDeltaLoad::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();

	auto &state = meta_pipeline.GetState();
	// This operator is the source of the current (probe) pipeline.
	state.SetPipelineSource(current, *this);
	if (!children.empty()) {
		GetDeltaFileList(*bind_data).ResetBuildExecution();
		// Materialize the reconciled file list before the multi-file source starts. DuckDB's
		// multi-file reader snapshots traversal state and is not safe to race with list growth: doing
		// so can rescan a path without its DV metadata, and can strand a source waiting for an append on
		// repeated execution. The ordinary child meta-pipeline installs the same build-before-probe
		// dependency used by hash joins and CTE materialization.
		auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
		child_meta_pipeline.Build(children[0]);
	}
}

} // namespace duckdb
