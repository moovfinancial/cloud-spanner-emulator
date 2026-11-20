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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_NULL_PREDICATES_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_NULL_PREDICATES_H_

#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Appends the conjuncts of 'expr' to 'conjuncts'.
void Conjuncts(const googlesql::ResolvedExpr* expr,
               std::vector<const googlesql::ResolvedExpr*>* conjuncts);

// Returns the column of 'expr' if it is `column IS NOT NULL`, or nullptr.
const googlesql::ResolvedColumn* IsNotNullColumn(
    const googlesql::ResolvedExpr* expr);

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_NULL_PREDICATES_H_
