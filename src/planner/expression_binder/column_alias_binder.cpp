#include "duckdb/planner/expression_binder/column_alias_binder.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// alias map will be modified during initial select column list binding.
ColumnAliasBinder::ColumnAliasBinder(BoundSelectNode &node, const case_insensitive_map_t<idx_t> &alias_map)
    : node(node), alias_map(alias_map), in_alias(false) {
}

bool ColumnAliasBinder::HasAlias(const std::string &column_name) {
	auto alias_entry = alias_map.find(column_name);
	return ((alias_entry != alias_map.end()));
}

// Get the underlying expression for someone referencing an alias. Returns a string on error.
string ColumnAliasBinder::GetRealExpression(const std::string &column_name, unique_ptr<ParsedExpression> &expr) {
	auto alias_entry = alias_map.find(column_name);
	if (alias_entry == alias_map.end()) {
		return StringUtil::Format("Alias %s is not found.", column_name);
	}

	// found an alias: bind the alias expression
	D_ASSERT(!in_alias);
	expr = node.original_expressions[alias_entry->second]->Copy();
	return string();
}

BindResult ColumnAliasBinder::BindAlias(ExpressionBinder &enclosing_binder, ColumnRefExpression &expr, idx_t depth,
                                        bool root_expression) {
	if (expr.IsQualified()) {
		return BindResult(StringUtil::Format("Alias %s cannot be qualified.", expr.ToString()));
	}

	unique_ptr<ParsedExpression> expression;
	auto alias_error = GetRealExpression(expr.column_names[0], expression);
	if (!alias_error.empty()) {
		return BindResult(alias_error);
	}

	in_alias = true;
	auto result = enclosing_binder.BindExpression(&expression, depth, root_expression);
	in_alias = false;
	return result;
}

} // namespace duckdb
