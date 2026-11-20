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

#include "backend/query/null_predicates.h"

#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Returns 'expr' as a call of the function 'name', or nullptr.
const googlesql::ResolvedFunctionCall* CallOf(
    const googlesql::ResolvedExpr* expr, const char* name) {
  if (expr == nullptr || !expr->Is<googlesql::ResolvedFunctionCall>()) {
    return nullptr;
  }
  const auto* call = expr->GetAs<googlesql::ResolvedFunctionCall>();
  return call->function()->Name() == name ? call : nullptr;
}

}  // namespace

void Conjuncts(const googlesql::ResolvedExpr* expr,
               std::vector<const googlesql::ResolvedExpr*>* conjuncts) {
  if (const auto* call = CallOf(expr, "$and")) {
    for (const auto& arg : call->argument_list()) {
      Conjuncts(arg.get(), conjuncts);
    }
    return;
  }
  conjuncts->push_back(expr);
}

const googlesql::ResolvedColumn* IsNotNullColumn(
    const googlesql::ResolvedExpr* expr) {
  const auto* not_call = CallOf(expr, "$not");
  if (not_call == nullptr || not_call->argument_list_size() != 1) {
    return nullptr;
  }
  const auto* is_null = CallOf(not_call->argument_list(0), "$is_null");
  if (is_null == nullptr || is_null->argument_list_size() != 1 ||
      !is_null->argument_list(0)->Is<googlesql::ResolvedColumnRef>()) {
    return nullptr;
  }
  return &is_null->argument_list(0)
              ->GetAs<googlesql::ResolvedColumnRef>()
              ->column();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
