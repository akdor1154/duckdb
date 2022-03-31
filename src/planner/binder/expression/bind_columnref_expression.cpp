#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/positional_reference_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> ExpressionBinder::QualifyColumnName(const string &column_name, string &error_message, bool prioritise_current_aliases) {
	auto using_binding = binder.bind_context.GetUsingBinding(column_name);
	if (using_binding) {
		// we are referencing a USING column
		// check if we can refer to one of the base columns directly
		unique_ptr<Expression> expression;
		if (!using_binding->primary_binding.empty()) {
			// we can! just assign the table name and re-bind
			return make_unique<ColumnRefExpression>(column_name, using_binding->primary_binding);
		} else {
			// // we cannot! we need to bind this as a coalesce between all the relevant columns
			auto coalesce = make_unique<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
			for (auto &entry : using_binding->bindings) {
				coalesce->children.push_back(make_unique<ColumnRefExpression>(column_name, entry));
			}
			return move(coalesce);
		}
	}

	// these could be std::optional given cpp17?
	unique_ptr<ParsedExpression> matched_from_macro(nullptr);
	unique_ptr<ParsedExpression> matched_from_alias(nullptr);
	unique_ptr<ParsedExpression> matched_from_column(nullptr);

	// This preserves existing behaviour.
	// TODO: change behaviour to error on ambiguity.
	if (binder.macro_binding != nullptr && binder.macro_binding->HasMatchingBinding(column_name)) {
		D_ASSERT(!binder.macro_binding->alias.empty());
		matched_from_macro = make_unique<ColumnRefExpression>(column_name, binder.macro_binding->alias);
	}

	if (binder.column_alias_binder && binder.column_alias_binder->HasAlias(column_name)) {
		unique_ptr<ParsedExpression> real_expression;
		auto alias_error = binder.column_alias_binder->GetRealExpression(column_name, real_expression);
		if (!alias_error.empty()) {
			throw new InternalException(
			    StringUtil::Format("Alias binding should have worked, but failed: %s", alias_error));
		}
		matched_from_alias = move(real_expression);
	}

	{
		string table_name = binder.bind_context.GetMatchingBinding(column_name);
		if (!table_name.empty()) {
			matched_from_column = binder.bind_context.CreateColumnReference(table_name, column_name);
		}
	}

	if (matched_from_macro) {
		// priority to macro parameter bindings TODO: throw a warning when this name conflicts
		return matched_from_macro;
	} else if (matched_from_column) {
		return matched_from_column;
	} else if (matched_from_alias) {
		return matched_from_alias;
	} else {
		auto similar_bindings = binder.bind_context.GetSimilarBindings(column_name);
		string candidate_str = StringUtil::CandidatesMessage(similar_bindings, "Candidate bindings");
		error_message =
		    StringUtil::Format("Referenced column \"%s\" not found in FROM clause!%s", column_name, candidate_str);
		return nullptr;
	}
}

void ExpressionBinder::QualifyColumnNames(unique_ptr<ParsedExpression> &expr, bool prioritise_current_aliases) {
	switch (expr->type) {
	case ExpressionType::COLUMN_REF: {
		auto &colref = (ColumnRefExpression &)*expr;
		string error_message;
		auto new_expr = QualifyColumnName(colref, error_message, prioritise_current_aliases);
		if (new_expr) {
			if (!expr->alias.empty()) {
				new_expr->alias = expr->alias;
			}
			expr = move(new_expr);
		}
		break;
	}
	case ExpressionType::POSITIONAL_REFERENCE: {
		auto &ref = (PositionalReferenceExpression &)*expr;
		if (ref.alias.empty()) {
			string table_name, column_name;
			auto error = binder.bind_context.BindColumn(ref, table_name, column_name);
			if (error.empty()) {
				ref.alias = column_name;
			}
		}
		break;
	}
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { QualifyColumnNames(child, prioritise_current_aliases); });
}

void ExpressionBinder::QualifyColumnNames(Binder &binder, unique_ptr<ParsedExpression> &expr, bool prioritise_current_aliases) {
	WhereBinder where_binder(binder, binder.context);
	where_binder.QualifyColumnNames(expr, prioritise_current_aliases);
}

unique_ptr<ParsedExpression> ExpressionBinder::CreateStructExtract(unique_ptr<ParsedExpression> base,
                                                                   string field_name) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(move(base));
	children.push_back(make_unique_base<ParsedExpression, ConstantExpression>(Value(move(field_name))));
	auto extract_fun = make_unique<FunctionExpression>("struct_extract", move(children));
	return move(extract_fun);
}

unique_ptr<ParsedExpression> ExpressionBinder::QualifyColumnName(ColumnRefExpression &colref, string &error_message, bool prioritise_current_aliases) {
	idx_t column_parts = colref.column_names.size();
	// column names can have an arbitrary amount of dots
	// here is how the resolution works:
	if (column_parts == 1) {
		// no dots (i.e. "part1")
		// -> part1 refers to a column
		// check if we can qualify the column name with the table name
		return QualifyColumnName(colref.GetColumnName(), error_message, prioritise_current_aliases);
	} else if (column_parts == 2) {
		// one dot (i.e. "part1.part2")
		// EITHER:
		// -> part1 is a table, part2 is a column
		// -> part1 is a column, part2 is a property of that column (i.e. struct_extract)

		// first check if part1 is a table
		if (binder.HasMatchingBinding(colref.column_names[0], colref.column_names[1], error_message)) {
			// it is! return the colref directly
			return binder.bind_context.CreateColumnReference(colref.column_names[0], colref.column_names[1]);
		} else {
			// otherwise check if we can turn this into a struct extract
			auto new_colref = make_unique<ColumnRefExpression>(colref.column_names[0]);
			string other_error;
			auto qualified_colref = QualifyColumnName(colref.column_names[0], other_error, prioritise_current_aliases);
			if (!qualified_colref) {
				// we could not! bail
				return nullptr;
			}
			// we could: create a struct extract
			return CreateStructExtract(move(qualified_colref), colref.column_names[1]);
		}
	} else {
		// two or more dots (i.e. "part1.part2.part3.part4...")
		// -> part1 is a schema, part2 is a table, part3 is a column name, part4 and beyond are struct fields
		// -> part1 is a table, part2 is a column name, part3 and beyond are struct fields
		// -> part1 is a column, part2 and beyond are struct fields

		// we always prefer the most top-level view
		// i.e. in case of multiple resolution options, we resolve in order:
		// -> 1. resolve "part1" as a schema
		// -> 2. resolve "part1" as a table
		// -> 3. resolve "part1" as a column

		unique_ptr<ParsedExpression> result_expr;
		idx_t struct_extract_start;
		// first check if part1 is a schema
		if (binder.HasMatchingBinding(colref.column_names[0], colref.column_names[1], colref.column_names[2],
		                              error_message)) {
			// it is! the column reference is "schema.table.column"
			// any additional fields are turned into struct_extract calls
			result_expr = binder.bind_context.CreateColumnReference(colref.column_names[0], colref.column_names[1],
			                                                        colref.column_names[2]);
			struct_extract_start = 3;
		} else if (binder.HasMatchingBinding(colref.column_names[0], colref.column_names[1], error_message)) {
			// part1 is a table
			// the column reference is "table.column"
			// any additional fields are turned into struct_extract calls
			result_expr = binder.bind_context.CreateColumnReference(colref.column_names[0], colref.column_names[1]);
			struct_extract_start = 2;
		} else {
			// part1 could be a column
			string col_error;
			result_expr = QualifyColumnName(colref.column_names[0], col_error, prioritise_current_aliases);
			if (!result_expr) {
				// it is not! return the error
				return nullptr;
			}
			// it is! add the struct extract calls
			struct_extract_start = 1;
		}
		for (idx_t i = struct_extract_start; i < colref.column_names.size(); i++) {
			result_expr = CreateStructExtract(move(result_expr), colref.column_names[i]);
		}
		return result_expr;
	}
}

BindResult ExpressionBinder::BindExpression(ColumnRefExpression &colref_p, idx_t depth) {
	if (binder.GetBindingMode() == BindingMode::EXTRACT_NAMES) {
		return BindResult(make_unique<BoundConstantExpression>(Value(LogicalType::SQLNULL)));
	}
	string error_message;
	auto expr = QualifyColumnName(colref_p, error_message, false);
	if (!expr) {
		return BindResult(binder.FormatError(colref_p, error_message));
	}
	if (expr->type != ExpressionType::COLUMN_REF) {
		return BindExpression(&expr, depth);
	}
	auto &colref = (ColumnRefExpression &)*expr;
	D_ASSERT(colref.column_names.size() == 2 || colref.column_names.size() == 3);
	auto &table_name = colref.column_names.size() == 3 ? colref.column_names[1] : colref.column_names[0];
	// individual column reference
	// resolve to either a base table or a subquery expression
	// if it was a macro parameter, let macro_binding bind it to the argument
	BindResult result;
	if (binder.macro_binding && table_name == binder.macro_binding->alias) {
		result = binder.macro_binding->Bind(colref, depth);
	} else {
		result = binder.bind_context.BindColumn(colref, depth);
	}
	if (!result.HasError()) {
		BoundColumnReferenceInfo ref;
		ref.name = colref.column_names.back();
		ref.query_location = colref.query_location;
		bound_columns.push_back(move(ref));
	} else {
		result.error = binder.FormatError(colref_p, result.error);
	}
	return result;
}

} // namespace duckdb
