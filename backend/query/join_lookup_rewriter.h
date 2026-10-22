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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_JOIN_LOOKUP_REWRITER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_JOIN_LOOKUP_REWRITER_H_

#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Rewrites an inner or left join whose right input is a table scan, joined by
// equality on the leading column of the table's primary key or of one of its
// indexes, into a lateral join. The right input then filters on the left
// column as a correlated value, so each left row reads only its matching keys
// instead of the join reading the whole table.
//
// The join keeps its condition, so the result does not change. Only joins
// whose left input is narrowed by an equality filter are rewritten: a lookup
// per row of a large left input costs more than one read of the table.
class JoinLookupRewriter : public googlesql::ResolvedASTDeepCopyVisitor {
 public:
  absl::Status VisitResolvedFilterScan(
      const googlesql::ResolvedFilterScan* node) override;
  absl::Status VisitResolvedJoinScan(
      const googlesql::ResolvedJoinScan* node) override;

 private:
  // Filters of the enclosing filter scans, innermost last.
  std::vector<const googlesql::ResolvedExpr*> enclosing_filters_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_JOIN_LOOKUP_REWRITER_H_
