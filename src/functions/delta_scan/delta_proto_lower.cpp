//===----------------------------------------------------------------------===//
// delta_proto_lower.cpp — proto schema/expression -> DuckDB (Phase D: S1 + E1, growing into E2).
//===----------------------------------------------------------------------===//
#include "functions/delta_scan/delta_proto_lower.hpp"

#include "delta_kernel.hpp" // delta::DeltaError

#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

#include <string>

namespace duckdb {

namespace kschema = ::delta::kernel::schema;
namespace kexpr = ::delta::kernel::expressions;

[[noreturn]] static void Unsupported(const std::string &what) {
	throw ::delta::DeltaError("DeltaPlanBuilder lowering: unsupported " + what);
}

//===----------------------------------------------------------------------===//
// S1 — schema types
//===----------------------------------------------------------------------===//

static LogicalType PrimitiveToDuckDB(const kschema::PrimitiveType &p) {
	using kschema::SimplePrimitiveType;
	if (p.has_decimal()) {
		const auto &d = p.decimal();
		return LogicalType::DECIMAL(static_cast<uint8_t>(d.precision()), static_cast<uint8_t>(d.scale()));
	}
	switch (p.simple()) {
	case kschema::SIMPLE_PRIMITIVE_TYPE_STRING:
		return LogicalType::VARCHAR;
	case kschema::SIMPLE_PRIMITIVE_TYPE_LONG:
		return LogicalType::BIGINT;
	case kschema::SIMPLE_PRIMITIVE_TYPE_INTEGER:
		return LogicalType::INTEGER;
	case kschema::SIMPLE_PRIMITIVE_TYPE_SHORT:
		return LogicalType::SMALLINT;
	case kschema::SIMPLE_PRIMITIVE_TYPE_BYTE:
		return LogicalType::TINYINT;
	case kschema::SIMPLE_PRIMITIVE_TYPE_FLOAT:
		return LogicalType::FLOAT;
	case kschema::SIMPLE_PRIMITIVE_TYPE_DOUBLE:
		return LogicalType::DOUBLE;
	case kschema::SIMPLE_PRIMITIVE_TYPE_BOOLEAN:
		return LogicalType::BOOLEAN;
	case kschema::SIMPLE_PRIMITIVE_TYPE_BINARY:
		return LogicalType::BLOB;
	case kschema::SIMPLE_PRIMITIVE_TYPE_DATE:
		return LogicalType::DATE;
	case kschema::SIMPLE_PRIMITIVE_TYPE_TIMESTAMP:
		return LogicalType::TIMESTAMP_TZ;
	case kschema::SIMPLE_PRIMITIVE_TYPE_TIMESTAMP_NTZ:
		return LogicalType::TIMESTAMP;
	default:
		Unsupported("primitive type");
	}
}

LogicalType SchemaTypeToDuckDB(const kschema::DataType &type) {
	switch (type.kind_case()) {
	case kschema::DataType::kPrimitive:
		return PrimitiveToDuckDB(type.primitive());
	case kschema::DataType::kArray:
		return LogicalType::LIST(SchemaTypeToDuckDB(type.array().element_type()));
	case kschema::DataType::kMap:
		return LogicalType::MAP(SchemaTypeToDuckDB(type.map().key_type()),
		                        SchemaTypeToDuckDB(type.map().value_type()));
	case kschema::DataType::kStruct: {
		child_list_t<LogicalType> children;
		for (const auto &f : type.struct_().fields()) {
			children.emplace_back(f.name(), SchemaTypeToDuckDB(f.data_type()));
		}
		return LogicalType::STRUCT(std::move(children));
	}
	default:
		Unsupported("data type kind");
	}
}

void SchemaToDuckDB(const kschema::StructType &schema, vector<string> &names, vector<LogicalType> &types) {
	for (const auto &f : schema.fields()) {
		names.push_back(f.name());
		types.push_back(SchemaTypeToDuckDB(f.data_type()));
	}
}

//===----------------------------------------------------------------------===//
// E1 — expressions (core)
//===----------------------------------------------------------------------===//

Value ScalarToValue(const kexpr::Scalar &s) {
	switch (s.value_case()) {
	case kexpr::Scalar::kInteger:
		return Value::INTEGER(s.integer());
	case kexpr::Scalar::kLong:
		return Value::BIGINT(s.long_());
	case kexpr::Scalar::kShort:
		return Value::SMALLINT(static_cast<int16_t>(s.short_()));
	case kexpr::Scalar::kByte:
		return Value::TINYINT(static_cast<int8_t>(s.byte()));
	case kexpr::Scalar::kFloat:
		return Value::FLOAT(s.float_());
	case kexpr::Scalar::kDouble:
		return Value::DOUBLE(s.double_());
	case kexpr::Scalar::kString:
		return Value(s.string());
	case kexpr::Scalar::kBoolean:
		return Value::BOOLEAN(s.boolean());
	case kexpr::Scalar::kDate:
		return Value::DATE(date_t(s.date()));
	case kexpr::Scalar::kTimestamp:
		return Value::TIMESTAMPTZ(timestamp_tz_t(s.timestamp()));
	case kexpr::Scalar::kTimestampNtz:
		return Value::TIMESTAMP(timestamp_t(s.timestamp_ntz()));
	case kexpr::Scalar::kNull:
		return Value(SchemaTypeToDuckDB(s.null()));
	case kexpr::Scalar::kDecimal: {
		// Decimal bits are a little-endian i128 (see kernel proto); reconstruct as hugeint.
		const auto &d = s.decimal();
		const std::string &bits = d.bits();
		hugeint_t h(0);
		// bytes are LE two's-complement i128
		uint64_t lo = 0, hi = 0;
		for (size_t i = 0; i < bits.size() && i < 8; i++) {
			lo |= static_cast<uint64_t>(static_cast<uint8_t>(bits[i])) << (8 * i);
		}
		for (size_t i = 8; i < bits.size() && i < 16; i++) {
			hi |= static_cast<uint64_t>(static_cast<uint8_t>(bits[i])) << (8 * (i - 8));
		}
		h.lower = lo;
		h.upper = static_cast<int64_t>(hi);
		return Value::DECIMAL(h, static_cast<uint8_t>(d.decimal_type().precision()),
		                      static_cast<uint8_t>(d.decimal_type().scale()));
	}
	default:
		Unsupported("scalar literal kind");
	}
}

// A kernel column path [a, b, c] lowers to struct_extract(struct_extract("a", 'b'), 'c') — a
// top-level column reference, then nested struct-field access. (DuckDB parses `a['b']` to the same.)
static unique_ptr<ParsedExpression> ColumnPathToExpr(
    const ::google::protobuf::RepeatedPtrField<std::string> &path) {
	if (path.empty()) {
		Unsupported("empty column name");
	}
	unique_ptr<ParsedExpression> e = make_uniq<ColumnRefExpression>(path.Get(0));
	for (int i = 1; i < path.size(); i++) {
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(std::move(e));
		args.push_back(make_uniq<ConstantExpression>(Value(path.Get(i))));
		e = make_uniq<FunctionExpression>("struct_extract", std::move(args));
	}
	return e;
}

static unique_ptr<ParsedExpression> ColumnToExpr(const kexpr::ColumnName &col) {
	return ColumnPathToExpr(col.path());
}

// --- schema helpers for the struct-producing expressions (E2) --------------------------------------

namespace kschema_ns = ::delta::kernel::schema;

// The struct type behind a DataType, or null if it isn't a struct.
static const kschema_ns::StructType *AsStruct(const kschema_ns::DataType *dt) {
	if (dt && dt->kind_case() == kschema_ns::DataType::kStruct) {
		return &dt->struct_();
	}
	return nullptr;
}

// A field's column-mapping physical name (delta.columnMapping.physicalName), or its logical name.
static const std::string &PhysicalName(const kschema_ns::StructField &f) {
	auto it = f.metadata().find("delta.columnMapping.physicalName");
	if (it != f.metadata().end() && it->second.value_case() == kschema_ns::MetadataValue::kString) {
		return it->second.string();
	}
	return f.name();
}

// Navigate a column path through nested structs, returning the struct it resolves to (or throw).
static const kschema_ns::StructType &ResolvePathToStruct(
    const kschema_ns::StructType &schema, const ::google::protobuf::RepeatedPtrField<std::string> &path) {
	const kschema_ns::StructType *cur = &schema;
	for (const auto &seg : path) {
		const kschema_ns::StructField *found = nullptr;
		for (const auto &f : cur->fields()) {
			if (f.name() == seg) {
				found = &f;
				break;
			}
		}
		if (!found) {
			Unsupported("Transform input_path segment not found in input schema");
		}
		const auto *s = AsStruct(&found->data_type());
		if (!s) {
			Unsupported("Transform input_path segment is not a struct");
		}
		cur = s;
	}
	return *cur;
}

static ExpressionType BinaryPredType(kexpr::BinaryPredicateOp op) {
	switch (op) {
	case kexpr::BINARY_PREDICATE_OP_LESS_THAN:
		return ExpressionType::COMPARE_LESSTHAN;
	case kexpr::BINARY_PREDICATE_OP_GREATER_THAN:
		return ExpressionType::COMPARE_GREATERTHAN;
	case kexpr::BINARY_PREDICATE_OP_EQUAL:
		return ExpressionType::COMPARE_EQUAL;
	case kexpr::BINARY_PREDICATE_OP_DISTINCT:
		return ExpressionType::COMPARE_DISTINCT_FROM;
	default:
		Unsupported("binary predicate op"); // IN handled separately if needed
	}
}

static const char *BinaryExprFn(kexpr::BinaryExpressionOp op) {
	switch (op) {
	case kexpr::BINARY_EXPRESSION_OP_PLUS:
		return "+";
	case kexpr::BINARY_EXPRESSION_OP_MINUS:
		return "-";
	case kexpr::BINARY_EXPRESSION_OP_MULTIPLY:
		return "*";
	case kexpr::BINARY_EXPRESSION_OP_DIVIDE:
		return "/";
	default:
		Unsupported("binary expression op");
	}
}

// --- E2 struct-producing expressions ---------------------------------------------------------------

// Build the JSON-schema structure string that from_json() wants (mirrors kernel parse_json_structure).
static std::string ParseJsonStructure(const kschema_ns::DataType &dt) {
	if (const auto *st = AsStruct(&dt)) {
		std::string out = "{";
		bool first = true;
		for (const auto &f : st->fields()) {
			if (!first) {
				out += ",";
			}
			first = false;
			std::string name = f.name();
			std::string esc;
			for (char c : name) {
				if (c == '\\') {
					esc += "\\\\";
				} else if (c == '"') {
					esc += "\\\"";
				} else {
					esc += c;
				}
			}
			out += "\"" + esc + "\":" + ParseJsonStructure(f.data_type());
		}
		out += "}";
		return out;
	}
	return "\"" + SchemaTypeToDuckDB(dt).ToString() + "\"";
}

// struct_pack(name := expr, ...): a ParsedExpression whose children carry field-name aliases.
static unique_ptr<ParsedExpression> StructPack(vector<std::pair<std::string, unique_ptr<ParsedExpression>>> fields) {
	vector<unique_ptr<ParsedExpression>> args;
	for (auto &f : fields) {
		f.second->SetAlias(f.first);
		args.push_back(std::move(f.second));
	}
	return make_uniq<FunctionExpression>("struct_pack", std::move(args));
}

// Rebuild `base` (typed `source`) as `target`, renaming struct fields / list elements by position
// (column-mapping physical -> logical). Primitives pass through. NULL-guarded for structs.
static unique_ptr<ParsedExpression> RebuildField(unique_ptr<ParsedExpression> base,
                                                 const kschema_ns::DataType &source,
                                                 const kschema_ns::DataType &target);

static unique_ptr<ParsedExpression> RebuildStruct(unique_ptr<ParsedExpression> base,
                                                  const kschema_ns::StructType &source,
                                                  const kschema_ns::StructType &target) {
	if (source.fields_size() != target.fields_size()) {
		Unsupported("identity Transform struct field count mismatch");
	}
	// struct_pack(tgt := rebuild(base['src']), ...) guarded: NULL input struct -> NULL output.
	vector<std::pair<std::string, unique_ptr<ParsedExpression>>> parts;
	for (int i = 0; i < source.fields_size(); i++) {
		const auto &sf = source.fields(i);
		const auto &tf = target.fields(i);
		vector<unique_ptr<ParsedExpression>> ex;
		ex.push_back(base->Copy());
		ex.push_back(make_uniq<ConstantExpression>(Value(sf.name())));
		auto child = make_uniq<FunctionExpression>("struct_extract", std::move(ex));
		parts.emplace_back(tf.name(), RebuildField(std::move(child), sf.data_type(), tf.data_type()));
	}
	auto packed = StructPack(std::move(parts));

	// CASE WHEN base IS NOT NULL THEN struct_pack(...) ELSE NULL END
	auto is_not_null = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL);
	is_not_null->children.push_back(base->Copy());
	auto case_expr = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(is_not_null);
	check.then_expr = std::move(packed);
	case_expr->case_checks.push_back(std::move(check));
	case_expr->else_expr = make_uniq<ConstantExpression>(Value());
	return case_expr;
}

static unique_ptr<ParsedExpression> RebuildField(unique_ptr<ParsedExpression> base,
                                                 const kschema_ns::DataType &source,
                                                 const kschema_ns::DataType &target) {
	if (source.kind_case() == kschema_ns::DataType::kStruct &&
	    target.kind_case() == kschema_ns::DataType::kStruct) {
		return RebuildStruct(std::move(base), source.struct_(), target.struct_());
	}
	if (source.kind_case() == kschema_ns::DataType::kArray &&
	    target.kind_case() == kschema_ns::DataType::kArray) {
		// list_transform(base, __dk_x -> rebuild(__dk_x)) — but only if the element needs renaming.
		auto probe = RebuildField(make_uniq<ColumnRefExpression>("__dk_x"), source.array().element_type(),
		                          target.array().element_type());
		if (probe->GetExpressionType() == ExpressionType::COLUMN_REF) {
			return base; // element needs no rename (probe returned the bare column)
		}
		auto lambda = make_uniq<LambdaExpression>(make_uniq<ColumnRefExpression>("__dk_x"), std::move(probe));
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(std::move(base));
		args.push_back(std::move(lambda));
		return make_uniq<FunctionExpression>("list_transform", std::move(args));
	}
	return base; // primitive / no rename needed
}

// Lower a Transform: densify the sparse spec against the input schema and emit one output column per
// output-schema field (prepended fields, then per input field a passthrough unless replaced, then that
// field's insert/replace expressions). Mirrors the kernel arrow engine's evaluate_transform_expression.
static unique_ptr<ParsedExpression> LowerTransform(const kexpr::Transform &t,
                                                   const kschema_ns::DataType *expected,
                                                   const kschema_ns::StructType *input) {
	const auto *target = AsStruct(expected);
	if (!target) {
		Unsupported("Transform requires an expected struct output type");
	}
	if (!input) {
		Unsupported("Transform requires the input schema in context");
	}

	int ti = 0;
	auto next_tgt = [&](void) -> const kschema_ns::StructField & {
		if (ti >= target->fields_size()) {
			Unsupported("Transform: too few fields in output schema");
		}
		return target->fields(ti++);
	};

	vector<std::pair<std::string, unique_ptr<ParsedExpression>>> parts;

	// 1. Prepended fields: expressions over the top-level input.
	for (const auto &e : t.prepended_fields()) {
		const auto &tf = next_tgt();
		parts.emplace_back(tf.name(), LowerExpr(e, &tf.data_type(), input));
	}

	// 2. Walk the source struct fields in order (input_path struct, or the top-level input).
	unique_ptr<ParsedExpression> base_col;
	const kschema_ns::StructType *source = input;
	if (t.has_input_path()) {
		base_col = ColumnPathToExpr(t.input_path().path());
		source = &ResolvePathToStruct(*input, t.input_path().path());
	}

	int used_field_transforms = 0;
	for (const auto &sf : source->fields()) {
		auto ft_it = t.field_transforms().find(sf.name());
		const bool has_ft = ft_it != t.field_transforms().end();
		const bool is_replace = has_ft && ft_it->second.is_replace();

		if (!is_replace) {
			const auto &tf = next_tgt();
			// child = base['sf'] (nested) or bare column "sf" (top-level).
			unique_ptr<ParsedExpression> child;
			if (base_col) {
				vector<unique_ptr<ParsedExpression>> ex;
				ex.push_back(base_col->Copy());
				ex.push_back(make_uniq<ConstantExpression>(Value(sf.name())));
				child = make_uniq<FunctionExpression>("struct_extract", std::move(ex));
			} else {
				child = make_uniq<ColumnRefExpression>(sf.name());
			}
			parts.emplace_back(tf.name(), RebuildField(std::move(child), sf.data_type(), tf.data_type()));
		}
		if (has_ft) {
			for (const auto &e : ft_it->second.exprs()) {
				const auto &tf = next_tgt();
				parts.emplace_back(tf.name(), LowerExpr(e, &tf.data_type(), input));
			}
			used_field_transforms++;
		}
	}

	int required = 0;
	for (const auto &kv : t.field_transforms()) {
		if (!kv.second.optional()) {
			required++;
		}
	}
	if (used_field_transforms < required) {
		Unsupported("Transform: some non-optional field transforms reference invalid input fields");
	}
	if (ti != target->fields_size()) {
		Unsupported("Transform: output arity mismatch");
	}

	auto body = StructPack(std::move(parts));
	if (!base_col) {
		return body;
	}
	// NULL-guard on the source struct.
	auto is_not_null = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL);
	is_not_null->children.push_back(std::move(base_col));
	auto case_expr = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(is_not_null);
	check.then_expr = std::move(body);
	case_expr->case_checks.push_back(std::move(check));
	case_expr->else_expr = make_uniq<ConstantExpression>(Value());
	return case_expr;
}

unique_ptr<ParsedExpression> LowerExpr(const kexpr::Expression &expr, const kschema_ns::DataType *expected,
                                       const kschema_ns::StructType *input) {
	switch (expr.kind_case()) {
	case kexpr::Expression::kLiteral:
		return make_uniq<ConstantExpression>(ScalarToValue(expr.literal()));
	case kexpr::Expression::kColumn:
		return ColumnToExpr(expr.column());
	case kexpr::Expression::kPredicate:
		return LowerPredicate(expr.predicate(), input);
	case kexpr::Expression::kBinary: {
		const auto &b = expr.binary();
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(LowerExpr(b.left(), nullptr, input));
		args.push_back(LowerExpr(b.right(), nullptr, input));
		return make_uniq<FunctionExpression>(BinaryExprFn(b.op()), std::move(args));
	}
	case kexpr::Expression::kUnary: {
		const auto &u = expr.unary();
		if (u.op() != kexpr::UNARY_EXPRESSION_OP_TO_JSON) {
			Unsupported("unary expression op");
		}
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(LowerExpr(u.expr(), nullptr, input));
		return make_uniq<FunctionExpression>("to_json", std::move(args));
	}
	case kexpr::Expression::kVariadic: {
		const auto &v = expr.variadic();
		vector<unique_ptr<ParsedExpression>> args;
		for (const auto &e : v.exprs()) {
			args.push_back(LowerExpr(e, nullptr, input));
		}
		switch (v.op()) {
		case kexpr::VARIADIC_EXPRESSION_OP_COALESCE:
			return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(args));
		case kexpr::VARIADIC_EXPRESSION_OP_ARRAY:
			return make_uniq<FunctionExpression>("list_value", std::move(args));
		default:
			Unsupported("variadic expression op");
		}
	}
	case kexpr::Expression::kIfExpr: {
		const auto &i = expr.if_expr();
		auto case_expr = make_uniq<CaseExpression>();
		CaseCheck check;
		check.when_expr = LowerPredicate(i.condition(), input);
		check.then_expr = LowerExpr(i.then_expr(), expected, input);
		case_expr->case_checks.push_back(std::move(check));
		case_expr->else_expr = LowerExpr(i.else_expr(), expected, input);
		return case_expr;
	}
	case kexpr::Expression::kStructExpr: {
		const auto *st = AsStruct(expected);
		if (!st) {
			Unsupported("Struct expression without an expected struct type");
		}
		const auto &se = expr.struct_expr();
		if (st->fields_size() != se.exprs_size()) {
			Unsupported("Struct expression arity != expected struct fields");
		}
		vector<std::pair<std::string, unique_ptr<ParsedExpression>>> parts;
		for (int i = 0; i < se.exprs_size(); i++) {
			const auto &f = st->fields(i);
			parts.emplace_back(f.name(), LowerExpr(se.exprs(i), &f.data_type(), input));
		}
		return StructPack(std::move(parts));
	}
	case kexpr::Expression::kParseJson: {
		const auto &p = expr.parse_json();
		// from_json(json, '<structure>') — lenient JSON parse into the output struct.
		kschema_ns::DataType wrap;
		*wrap.mutable_struct_() = p.output_schema();
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(LowerExpr(p.json_expr(), nullptr, input));
		args.push_back(make_uniq<ConstantExpression>(Value(ParseJsonStructure(wrap))));
		return make_uniq<FunctionExpression>("from_json", std::move(args));
	}
	case kexpr::Expression::kMapToStruct: {
		const auto *st = AsStruct(expected);
		if (!st) {
			Unsupported("MapToStruct needs an expected struct type");
		}
		if (st->fields_size() == 0) {
			Unsupported("MapToStruct with an empty target struct");
		}
		const auto &m = expr.map_to_struct();
		auto map_expr = LowerExpr(m.map_expr(), nullptr, input);
		// struct_pack(field := CAST(map_extract(map, 'key')[1] AS type) | encode(...) for BLOB)
		vector<std::pair<std::string, unique_ptr<ParsedExpression>>> parts;
		for (const auto &f : st->fields()) {
			const std::string &key = PhysicalName(f);
			// map_extract(map, 'key')
			vector<unique_ptr<ParsedExpression>> me;
			me.push_back(map_expr->Copy());
			me.push_back(make_uniq<ConstantExpression>(Value(key)));
			auto extracted = make_uniq<FunctionExpression>("map_extract", std::move(me));
			// [1] -> list_extract(map_extract(...), 1)
			vector<unique_ptr<ParsedExpression>> le;
			le.push_back(std::move(extracted));
			le.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
			auto raw = make_uniq<FunctionExpression>("list_extract", std::move(le));

			LogicalType field_type = SchemaTypeToDuckDB(f.data_type());
			unique_ptr<ParsedExpression> val;
			if (field_type.id() == LogicalTypeId::BLOB) {
				// Binary partition values are the UTF-8 bytes of the string (encode), not a cast.
				vector<unique_ptr<ParsedExpression>> enc;
				enc.push_back(std::move(raw));
				val = make_uniq<FunctionExpression>("encode", std::move(enc));
			} else {
				val = make_uniq<CastExpression>(field_type, std::move(raw));
			}
			parts.emplace_back(f.name(), std::move(val));
		}
		return StructPack(std::move(parts));
	}
	case kexpr::Expression::kTransform:
		return LowerTransform(expr.transform(), expected, input);
	case kexpr::Expression::kOpaque:
		Unsupported("Opaque expression cannot be lowered");
	case kexpr::Expression::kUnknown:
		Unsupported("Unknown expression cannot be lowered");
	default:
		Unsupported("expression kind");
	}
}

unique_ptr<ParsedExpression> LowerPredicate(const kexpr::Predicate &pred, const kschema_ns::StructType *input) {
	switch (pred.kind_case()) {
	case kexpr::Predicate::kBooleanExpression:
		return LowerExpr(pred.boolean_expression(), nullptr, input);
	case kexpr::Predicate::kNot: {
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(LowerPredicate(pred.not_(), input));
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(args));
	}
	case kexpr::Predicate::kUnary: {
		const auto &u = pred.unary();
		if (u.op() != kexpr::UNARY_PREDICATE_OP_IS_NULL) {
			Unsupported("unary predicate op");
		}
		vector<unique_ptr<ParsedExpression>> args;
		args.push_back(LowerExpr(u.expr(), nullptr, input));
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, std::move(args));
	}
	case kexpr::Predicate::kBinary: {
		const auto &b = pred.binary();
		if (b.op() == kexpr::BINARY_PREDICATE_OP_IN) {
			// x IN <list>: COMPARE_IN over [x, <list-elements>...]. The RHS is a variadic/array expr.
			auto lhs = LowerExpr(b.left(), nullptr, input);
			auto rhs = LowerExpr(b.right(), nullptr, input);
			vector<unique_ptr<ParsedExpression>> args;
			args.push_back(std::move(lhs));
			args.push_back(std::move(rhs));
			return make_uniq<OperatorExpression>(ExpressionType::COMPARE_IN, std::move(args));
		}
		return make_uniq<ComparisonExpression>(BinaryPredType(b.op()), LowerExpr(b.left(), nullptr, input),
		                                       LowerExpr(b.right(), nullptr, input));
	}
	case kexpr::Predicate::kJunction: {
		const auto &j = pred.junction();
		ExpressionType et = (j.op() == kexpr::JUNCTION_PREDICATE_OP_OR) ? ExpressionType::CONJUNCTION_OR
		                                                                : ExpressionType::CONJUNCTION_AND;
		if (j.preds_size() == 0) {
			// empty AND = TRUE, empty OR = FALSE
			return make_uniq<ConstantExpression>(Value::BOOLEAN(j.op() != kexpr::JUNCTION_PREDICATE_OP_OR));
		}
		unique_ptr<ParsedExpression> acc = LowerPredicate(j.preds(0), input);
		for (int i = 1; i < j.preds_size(); i++) {
			acc = make_uniq<ConjunctionExpression>(et, std::move(acc), LowerPredicate(j.preds(i), input));
		}
		return acc;
	}
	case kexpr::Predicate::kOpaque:
		Unsupported("Opaque predicate cannot be lowered");
	case kexpr::Predicate::kUnknown:
		Unsupported("Unknown predicate cannot be lowered");
	default:
		Unsupported("predicate kind");
	}
}

} // namespace duckdb
