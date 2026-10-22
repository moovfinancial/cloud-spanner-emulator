//
// Copyright 2026 Google LLC
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

#include "backend/query/join_lookup_rewriter.h"

#include <memory>
#include <utility>
#include <vector>

#include "googlesql/public/types/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "backend/query/queryable_column.h"
#include "backend/query/queryable_table.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/table.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ColumnSet = absl::flat_hash_set<googlesql::ResolvedColumn>;

// Appends the conjuncts of 'expr' to 'conjuncts'.
void Conjuncts(const googlesql::ResolvedExpr* expr,
               std::vector<const googlesql::ResolvedExpr*>* conjuncts) {
  if (expr->Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expr->GetAs<googlesql::ResolvedFunctionCall>();
    if (call->function()->Name() == "$and") {
      for (const auto& arg : call->argument_list()) {
        Conjuncts(arg.get(), conjuncts);
      }
      return;
    }
  }
  conjuncts->push_back(expr);
}

// Returns the column 'expr' refers to in 'columns', or nullptr.
const googlesql::ResolvedColumn* LocalColumn(const googlesql::ResolvedExpr* expr,
                                             const ColumnSet& columns) {
  if (!expr->Is<googlesql::ResolvedColumnRef>()) {
    return nullptr;
  }
  const auto* ref = expr->GetAs<googlesql::ResolvedColumnRef>();
  if (ref->is_correlated() || !columns.contains(ref->column())) {
    return nullptr;
  }
  return &ref->column();
}

// Whether 'expr' has the same value for every row of a scan: a literal, a
// parameter, or a column of an enclosing query.
bool IsConstant(const googlesql::ResolvedExpr* expr) {
  return expr->Is<googlesql::ResolvedLiteral>() ||
         expr->Is<googlesql::ResolvedParameter>() ||
         (expr->Is<googlesql::ResolvedColumnRef>() &&
          expr->GetAs<googlesql::ResolvedColumnRef>()->is_correlated());
}

// Whether a conjunct of 'expr' compares a column of 'columns' with a constant
// by equality or IN.
bool HasConstantEquality(const googlesql::ResolvedExpr* expr,
                         const ColumnSet& columns) {
  std::vector<const googlesql::ResolvedExpr*> conjuncts;
  Conjuncts(expr, &conjuncts);
  for (const googlesql::ResolvedExpr* conjunct : conjuncts) {
    if (!conjunct->Is<googlesql::ResolvedFunctionCall>()) {
      continue;
    }
    const auto* call = conjunct->GetAs<googlesql::ResolvedFunctionCall>();
    const std::string& name = call->function()->Name();
    const auto& args = call->argument_list();
    if (name == "$equal" && args.size() == 2) {
      if ((LocalColumn(args[0].get(), columns) && IsConstant(args[1].get())) ||
          (LocalColumn(args[1].get(), columns) && IsConstant(args[0].get()))) {
        return true;
      }
    } else if ((name == "$in" || name == "$in_array") && !args.empty() &&
               LocalColumn(args[0].get(), columns)) {
      bool constant = true;
      for (int i = 1; i < args.size(); ++i) {
        constant = constant && IsConstant(args[i].get());
      }
      if (constant) {
        return true;
      }
    }
  }
  return false;
}

ColumnSet Columns(const googlesql::ResolvedScan* scan) {
  return ColumnSet(scan->column_list().begin(), scan->column_list().end());
}

// Whether 'scan' filters its rows by equality with a constant.
bool IsSelective(const googlesql::ResolvedScan* scan) {
  if (scan->Is<googlesql::ResolvedFilterScan>()) {
    const auto* filter = scan->GetAs<googlesql::ResolvedFilterScan>();
    return HasConstantEquality(filter->filter_expr(),
                               Columns(filter->input_scan())) ||
           IsSelective(filter->input_scan());
  }
  if (scan->Is<googlesql::ResolvedProjectScan>()) {
    return IsSelective(scan->GetAs<googlesql::ResolvedProjectScan>()->input_scan());
  }
  if (scan->Is<googlesql::ResolvedJoinScan>()) {
    return IsSelective(scan->GetAs<googlesql::ResolvedJoinScan>()->left_scan());
  }
  return false;
}

// Whether SQL equality of the type is the equality of its key values.
bool HasKeyEquality(const googlesql::Type* type) {
  return type->IsInt64() || type->IsString() || type->IsBytes() ||
         type->IsBool() || type->IsDate() || type->IsTimestamp();
}

// Whether a read of 'table' is narrowed by an equality on 'column': it leads
// the primary key or the key of an index.
bool LeadsAKey(const QueryableTable* table, const Column* column) {
  const backend::Table* wrapped = table->wrapped_table();
  if (!wrapped->primary_key().empty() &&
      wrapped->primary_key()[0]->column() == column) {
    return true;
  }
  for (const Index* index : wrapped->indexes()) {
    if (index->is_search_index() || index->is_vector_index() ||
        index->key_columns().empty()) {
      continue;
    }
    if (index->key_columns()[0]->column()->source_column() == column) {
      return true;
    }
  }
  return false;
}

}  // namespace

absl::Status JoinLookupRewriter::VisitResolvedFilterScan(
    const googlesql::ResolvedFilterScan* node) {
  enclosing_filters_.push_back(node->filter_expr());
  absl::Status status = CopyVisitResolvedFilterScan(node);
  enclosing_filters_.pop_back();
  return status;
}

absl::Status JoinLookupRewriter::VisitResolvedJoinScan(
    const googlesql::ResolvedJoinScan* node) {
  GOOGLESQL_RETURN_IF_ERROR(CopyVisitResolvedJoinScan(node));

  if (node->is_lateral() || node->join_expr() == nullptr ||
      (node->join_type() != googlesql::ResolvedJoinScan::INNER &&
       node->join_type() != googlesql::ResolvedJoinScan::LEFT) ||
      !node->right_scan()->Is<googlesql::ResolvedTableScan>()) {
    return absl::OkStatus();
  }
  const auto* scan = node->right_scan()->GetAs<googlesql::ResolvedTableScan>();
  const auto* table = dynamic_cast<const QueryableTable*>(scan->table());
  if (table == nullptr || scan->for_system_time_expr() != nullptr ||
      !scan->hint_list().empty() ||
      scan->column_index_list_size() != scan->column_list_size()) {
    return absl::OkStatus();
  }

  // An equality between a left column and a right column that leads a key.
  const ColumnSet left_columns = Columns(node->left_scan());
  const ColumnSet right_columns = Columns(scan);
  std::vector<const googlesql::ResolvedExpr*> conjuncts;
  Conjuncts(node->join_expr(), &conjuncts);
  const googlesql::ResolvedFunctionCall* lookup = nullptr;
  const googlesql::ResolvedColumn* left_column = nullptr;
  for (const googlesql::ResolvedExpr* conjunct : conjuncts) {
    if (!conjunct->Is<googlesql::ResolvedFunctionCall>()) {
      continue;
    }
    const auto* call = conjunct->GetAs<googlesql::ResolvedFunctionCall>();
    if (call->function()->Name() != "$equal" ||
        call->argument_list_size() != 2) {
      continue;
    }
    for (int side = 0; side < 2; ++side) {
      const googlesql::ResolvedColumn* left =
          LocalColumn(call->argument_list(side), left_columns);
      const googlesql::ResolvedColumn* right =
          LocalColumn(call->argument_list(1 - side), right_columns);
      if (left == nullptr || right == nullptr ||
          !left->type()->Equals(right->type()) ||
          !HasKeyEquality(right->type())) {
        continue;
      }
      int position = -1;
      for (int i = 0; i < scan->column_list_size(); ++i) {
        if (scan->column_list(i) == *right) {
          position = i;
        }
      }
      const auto* column = dynamic_cast<const QueryableColumn*>(
          table->GetColumn(scan->column_index_list(position)));
      if (column != nullptr && LeadsAKey(table, column->wrapped_column())) {
        lookup = call;
        left_column = left;
        break;
      }
    }
    if (lookup != nullptr) {
      break;
    }
  }
  if (lookup == nullptr) {
    return absl::OkStatus();
  }

  bool selective = IsSelective(node->left_scan());
  for (const googlesql::ResolvedExpr* filter : enclosing_filters_) {
    selective = selective || HasConstantEquality(filter, left_columns);
  }
  if (!selective) {
    return absl::OkStatus();
  }

  // The right input filters on the left column as a correlated value.
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<googlesql::ResolvedFunctionCall> filter,
                   ResolvedASTDeepCopyVisitor::Copy(lookup));
  std::vector<std::unique_ptr<const googlesql::ResolvedExpr>> args =
      filter->release_argument_list();
  for (auto& arg : args) {
    if (LocalColumn(arg.get(), left_columns) != nullptr) {
      arg = googlesql::MakeResolvedColumnRef(left_column->type(), *left_column,
                                             /*is_correlated=*/true);
    }
  }
  filter->set_argument_list(std::move(args));

  auto* join = GetUnownedTopOfStack<googlesql::ResolvedJoinScan>();
  std::unique_ptr<const googlesql::ResolvedScan> right = join->release_right_scan();
  std::vector<googlesql::ResolvedColumn> right_column_list = right->column_list();
  join->set_right_scan(googlesql::MakeResolvedFilterScan(
      right_column_list, std::move(right), std::move(filter)));
  join->set_is_lateral(true);
  join->add_parameter_list(googlesql::MakeResolvedColumnRef(
      left_column->type(), *left_column, /*is_correlated=*/false));
  return absl::OkStatus();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
