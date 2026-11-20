//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/query/index_hint_validator.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "googlesql/resolved_ast/resolved_node.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "backend/query/null_predicates.h"
#include "backend/query/queryable_table.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/updater/global_schema_names.h"
#include "common/errors.h"
#include "re2/re2.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Query hints using generated names of managed indexes are special-cased. The
// emulator and production name generators use different fingerprint algorithms,
// so the generated names have different suffixes. A hint using a managed
// index's name that was generated in production never matches the name of the
// emulator's index, and vice versa. In order to permit the use of production
// queries in the emulator, names generated in production are permitted in the
// emulator. However, in order to avoid the unintentional and unsupported use of
// emulator generated names in production, names generated in the emulator are
// not allowed in query hints, neither in production nor in the emulator.

// Returns true if an index name matches the signature of a generated managed
// index name. See also GlobalSchemaNames::GenerateManagedIndexName.
bool MatchesManagedIndexName(absl::string_view table_name,
                             absl::string_view index_name) {
  return RE2::FullMatch(index_name, absl::StrCat("IDX_", table_name, "_",
                                                 "\\w+", "_", "[0-9A-F]{16}"));
}

// Returns true if an index's name matches the name of an index created in the
// emulator with GlobalSchemaNames::GenerateManagedIndexName.
bool HasGeneratedEmulatorName(const Index* index) {
  if (!index->is_managed()) {
    return false;  // Only managed indexes have generated names.
  }
  auto columns = index->key_columns();
  std::vector<std::string> column_names;
  std::transform(
      columns.begin(), columns.end(), std::back_inserter(column_names),
      [](const KeyColumn* column) { return column->column()->Name(); });
  return index->Name() == GlobalSchemaNames::GenerateManagedIndexName(
                              index->indexed_table()->Name(), column_names,
                              index->is_null_filtered(), index->is_unique())
                              .value();
}

// What the filters of a table scan prove about one of its columns.
struct ColumnFilter {
  // A filter rejects rows where the column is NULL in a form that Cloud
  // Spanner recognizes: `col IS NOT NULL`, or `col = @param` or
  // `col = <non-null literal>`. The Cloud Spanner docs accept
  // `col = <literal>`. `col = @param` also rejects NULL rows when the
  // parameter is NULL, because the comparison is then NULL.
  bool not_null = false;
  // A filter is `col IN UNNEST(...)`. Cloud Spanner does not recognize it as
  // a filter that rejects NULLs.
  bool in_unnest = false;
};

bool IsColumn(const googlesql::ResolvedExpr* expr,
              const googlesql::ResolvedColumn& column) {
  return expr->Is<googlesql::ResolvedColumnRef>() &&
         expr->GetAs<googlesql::ResolvedColumnRef>()->column() == column;
}

bool IsNonNullConstant(const googlesql::ResolvedExpr* expr) {
  return expr->Is<googlesql::ResolvedParameter>() ||
         (expr->Is<googlesql::ResolvedLiteral>() &&
          !expr->GetAs<googlesql::ResolvedLiteral>()->value().is_null());
}

ColumnFilter FilterOn(
    const googlesql::ResolvedTableScan* scan, absl::string_view column_name,
    const std::vector<const googlesql::ResolvedExpr*>& filters) {
  ColumnFilter result;
  for (const googlesql::ResolvedColumn& column : scan->column_list()) {
    if (!absl::EqualsIgnoreCase(column.name(), column_name)) {
      continue;
    }
    for (const googlesql::ResolvedExpr* filter : filters) {
      if (const auto* not_null = IsNotNullColumn(filter)) {
        result.not_null = result.not_null || *not_null == column;
        continue;
      }
      if (!filter->Is<googlesql::ResolvedFunctionCall>()) {
        continue;
      }
      const auto* call = filter->GetAs<googlesql::ResolvedFunctionCall>();
      const std::string& name = call->function()->Name();
      const auto& args = call->argument_list();
      if (name == "$equal" && args.size() == 2 &&
          ((IsColumn(args[0].get(), column) &&
            IsNonNullConstant(args[1].get())) ||
           (IsColumn(args[1].get(), column) &&
            IsNonNullConstant(args[0].get())))) {
        result.not_null = true;
      } else if (name == "$in_array" && !args.empty() &&
                 IsColumn(args[0].get(), column)) {
        result.in_unnest = true;
      }
    }
  }
  return result;
}

}  // namespace

absl::Status IndexHintValidator::VisitResolvedTableScan(
    const googlesql::ResolvedTableScan* table_scan) {
  // Visit child nodes first.
  GOOGLESQL_RETURN_IF_ERROR(googlesql::ResolvedASTVisitor::DefaultVisit(table_scan));

  std::vector<const googlesql::ResolvedNode*> child_nodes;
  table_scan->GetChildNodes(&child_nodes);

  for (const googlesql::ResolvedNode* child_node : child_nodes) {
    if (child_node->node_kind() == googlesql::RESOLVED_OPTION) {
      const googlesql::ResolvedOption* hint =
          child_node->GetAs<googlesql::ResolvedOption>();
      if ((absl::EqualsIgnoreCase(hint->qualifier(), "spanner") ||
           hint->qualifier().empty()) &&
          absl::EqualsIgnoreCase(hint->name(), "force_index")) {
        // We should expect only one hint per table scan as multiple hints per
        // node is not allowed and would've been rejected by the HintValidator.
        GOOGLESQL_RET_CHECK_EQ(hint->value()->node_kind(), googlesql::RESOLVED_LITERAL);
        const googlesql::Value& value =
            hint->value()->GetAs<googlesql::ResolvedLiteral>()->value();
        GOOGLESQL_RET_CHECK(value.type()->IsString());
        index_hints_map_[table_scan] = value.string_value();
        break;
      }
    }
  }

  return absl::OkStatus();
}

absl::Status IndexHintValidator::ValidateIndexesForTables() {
  for (auto [table_scan, index_name] : index_hints_map_) {
    if (absl::EqualsIgnoreCase(index_name, "_base_table")) {
      continue;
    }
    auto table = table_scan->table();
    auto query_table = table->GetAs<QueryableTable>();
    auto schema_table = query_table->wrapped_table();
    const auto* index = schema_table->FindIndex(
        schema_table->FindIndexQualifiedName(index_name));

    // See comments above regarding special-casing of managed indexes.
    if (index == nullptr) {
      if (MatchesManagedIndexName(schema_table->Name(), index_name)) {
        return absl::OkStatus();
      }
      return error::QueryHintIndexNotFound(schema_table->Name(), index_name);
    } else if (HasGeneratedEmulatorName(index)) {
      return error::QueryHintManagedIndexNotSupported(index_name);
    }

    if (index->is_search_index()) {
      if (!allow_search_indexes_in_transaction_) {
        return error::SearchIndexNotUsable(
            index_name,
            "is a SEARCH index type which is not supported for transactional "
            "queries by default");
      }

      // FOR UPDATE queries are not supported on search indexes.
      if (in_select_for_update_query_) {
        return error::ForUpdateUnsupportedInSearchQueries();
      }

      if (in_partition_query_) {
        // Not allowed in batch query.
        return error::SearchIndexNotUsable(
            index_name,
            "is a SEARCH index type which is not supported for partitioned "
            "queries");
      }
    }
    if (index->is_null_filtered()) {
      // If any of the index's columns are nullable, then it is not indexing
      // the full table. The index can only be used if the query's filters
      // reject NULLs in each of those columns.
      const std::vector<const googlesql::ResolvedExpr*> no_filters;
      auto it = filters_map_.find(table_scan);
      const auto& filters = it == filters_map_.end() ? no_filters : it->second;
      bool unproven = false;
      for (const auto* key_column : index->key_columns()) {
        const auto* source_column = key_column->column()->source_column();
        if (!source_column->is_nullable()) {
          continue;
        }
        ColumnFilter filter =
            FilterOn(table_scan, source_column->Name(), filters);
        // Cloud Spanner rejects this form, so the check cannot be disabled.
        if (filter.in_unnest && !filter.not_null) {
          return error::NullFilteredIndexUnusableForInUnnest(
              index_name, source_column->Name());
        }
        unproven = unproven || !filter.not_null;
      }
      if (unproven && !disable_null_filtered_index_check_) {
        return error::NullFilteredIndexUnusable(index_name);
      }
    }
  }
  return absl::OkStatus();
}

absl::Status IndexHintValidator::VisitResolvedFilterScan(
    const googlesql::ResolvedFilterScan* scan) {
  AddFilters(scan->input_scan(), scan->filter_expr());
  return googlesql::ResolvedASTVisitor::DefaultVisit(scan);
}

void IndexHintValidator::AddFilters(const googlesql::ResolvedScan* scan,
                                    const googlesql::ResolvedExpr* filter) {
  if (scan == nullptr || filter == nullptr) {
    return;
  }
  if (scan->Is<googlesql::ResolvedTableScan>()) {
    Conjuncts(filter,
              &filters_map_[scan->GetAs<googlesql::ResolvedTableScan>()]);
  } else if (scan->Is<googlesql::ResolvedFilterScan>()) {
    AddFilters(scan->GetAs<googlesql::ResolvedFilterScan>()->input_scan(),
               filter);
  } else if (scan->Is<googlesql::ResolvedProjectScan>()) {
    // Computed columns get new column ids, so only pass-through columns match.
    AddFilters(scan->GetAs<googlesql::ResolvedProjectScan>()->input_scan(),
               filter);
  } else if (scan->Is<googlesql::ResolvedJoinScan>()) {
    const auto* join = scan->GetAs<googlesql::ResolvedJoinScan>();
    AddFilters(join->left_scan(), filter);
    AddFilters(join->right_scan(), filter);
  }
}

absl::Status IndexHintValidator::VisitResolvedQueryStmt(
    const googlesql::ResolvedQueryStmt* stmt) {
  // Visit children first to collect all hints.
  GOOGLESQL_RETURN_IF_ERROR(googlesql::ResolvedASTVisitor::DefaultVisit(stmt));
  // Validate all index hints.
  return ValidateIndexesForTables();
}

absl::Status IndexHintValidator::VisitResolvedInsertStmt(
    const googlesql::ResolvedInsertStmt* stmt) {
  // Visit children first to collect all hints.
  GOOGLESQL_RETURN_IF_ERROR(googlesql::ResolvedASTVisitor::DefaultVisit(stmt));
  // The target table should not have any hints (not allowed by GoogleSQL).
  GOOGLESQL_RET_CHECK(!index_hints_map_.contains(stmt->table_scan()));
  return ValidateIndexesForTables();
}

absl::Status IndexHintValidator::VisitResolvedUpdateStmt(
    const googlesql::ResolvedUpdateStmt* stmt) {
  AddFilters(stmt->table_scan(), stmt->where_expr());
  // Visit children first to collect all hints.
  GOOGLESQL_RETURN_IF_ERROR(googlesql::ResolvedASTVisitor::DefaultVisit(stmt));
  return ValidateIndexesForTables();
}

absl::Status IndexHintValidator::VisitResolvedDeleteStmt(
    const googlesql::ResolvedDeleteStmt* stmt) {
  AddFilters(stmt->table_scan(), stmt->where_expr());
  // Visit children first to collect all hints.
  GOOGLESQL_RETURN_IF_ERROR(googlesql::ResolvedASTVisitor::DefaultVisit(stmt));
  return ValidateIndexesForTables();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
