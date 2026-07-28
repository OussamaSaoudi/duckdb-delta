//===----------------------------------------------------------------------===//
// delta_proto_lower.hpp — proto schema/expression -> DuckDB type/expression lowering.
//
// The leaf conversions DeltaPlanBuilder builds on (Phase D, steps S1/E1/E2):
//   - SchemaToDuckDB:  schema::StructType  -> DuckDB LogicalType (+ column names)
//   - LowerExpr:       expressions::Expression -> DuckDB ParsedExpression
//   - LowerPredicate:  expressions::Predicate  -> DuckDB ParsedExpression (boolean)
//
// C++17 island (includes the generated proto structs). Unsupported constructs throw
// delta::DeltaError so unsupported plans fail explicitly.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_expression.hpp"

#include "expressions.pb.h"
#include "schema.pb.h"

#include <string>

namespace duckdb {

//! Lower a kernel schema data type to a DuckDB LogicalType. Throws delta::DeltaError on an
//! unsupported/unset kind.
LogicalType SchemaTypeToDuckDB(const ::delta::kernel::schema::DataType &type);

//! Lower a kernel scalar literal to a DuckDB Value. Throws delta::DeltaError on an unsupported kind.
Value ScalarToValue(const ::delta::kernel::expressions::Scalar &scalar);

//! Lower a kernel struct schema to parallel (name, type) columns. Throws on any unsupported field.
void SchemaToDuckDB(const ::delta::kernel::schema::StructType &schema, vector<string> &names,
                    vector<LogicalType> &types);

//! Lower a kernel IR expression to a DuckDB unbound ParsedExpression. Throws on an unsupported kind.
//!
//! `expected` (may be null) is the expression's output type — it shapes struct-producing expressions
//! (Struct/MapToStruct/ParseJson/Transform need to know their target field names/types). `input` (may
//! be null) is the schema of the relation the expression is evaluated over — needed to lower identity
//! Transforms (which rename the input struct's physical field names to the output's logical names).
unique_ptr<ParsedExpression> LowerExpr(const ::delta::kernel::expressions::Expression &expr,
                                       const ::delta::kernel::schema::DataType *expected = nullptr,
                                       const ::delta::kernel::schema::StructType *input = nullptr);

//! Lower a kernel IR predicate to a DuckDB unbound boolean ParsedExpression. Throws on unsupported.
//! `input` (may be null) is the schema of the relation the predicate is evaluated over.
unique_ptr<ParsedExpression> LowerPredicate(const ::delta::kernel::expressions::Predicate &pred,
                                            const ::delta::kernel::schema::StructType *input = nullptr);

} // namespace duckdb
