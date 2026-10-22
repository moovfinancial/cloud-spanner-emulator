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

#include "backend/query/queryable_table.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"  //
#include "absl/types/span.h"
#include "backend/access/read.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/column_expression_analysis_cache.h"
#include "backend/query/queryable_column.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "common/constants.h"
#include "common/feature_flags.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// An implementation of EvaluatorTableIterator which wraps a RowCursor.
//
// Used by QueryableTable::CreateEvaluatorTableIterator.
class RowCursorEvaluatorTableIterator
    : public googlesql::EvaluatorTableIterator {
 public:
  explicit RowCursorEvaluatorTableIterator(std::unique_ptr<RowCursor> cursor)
      : cursor_(std::move(cursor)) {
    values_.reserve(cursor_->NumColumns());
    for (int i = 0; i < cursor_->NumColumns(); ++i) {
      values_.push_back(googlesql::values::Null(cursor_->ColumnType(i)));
    }
  }

  int NumColumns() const override { return cursor_->NumColumns(); }

  std::string GetColumnName(int i) const override {
    return cursor_->ColumnName(i);
  }

  const googlesql::Type* GetColumnType(int i) const override {
    return cursor_->ColumnType(i);
  }

  bool NextRow() override {
    if (cursor_->Next()) {
      for (int i = 0; i < cursor_->NumColumns(); ++i) {
        values_[i] = cursor_->ColumnValue(i);
      }
      return true;
    } else {
      return false;
    }
  }

  const googlesql::Value& GetValue(int i) const override { return values_[i]; }

  absl::Status Status() const override { return cursor_->Status(); }

  // Cancel is best-effort and not required.
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  // The wrapped RowCursor.
  std::unique_ptr<RowCursor> cursor_;

  // Values of the current row. EvaluatorTableIterator::GetValue need to return
  // a reference so we need to buffer the values instead of simply delegate to
  // RowCursor::ColumnValue.
  std::vector<googlesql::Value> values_;
};

namespace {

// The most keys a scan is narrowed to. A filter that would pass this is left
// out, and the read stays as wide as the filters before it made it.
constexpr size_t kMaxKeysFromFilters = 10000;

// Narrowing is on unless the process is started with this variable set, so a
// result can be compared against a whole-table read.
bool KeyFilterPushdownEnabled() {
  static const bool enabled =
      std::getenv("SPANNER_EMULATOR_DISABLE_KEY_FILTER_PUSHDOWN") == nullptr;
  return enabled;
}

}  // namespace

// An EvaluatorTableIterator that reads when its first row is asked for, so
// that the filters the evaluator offers before then can narrow the read from
// the whole table to the keys that can match.
//
// Only equality and IN filters are used, on the leading primary key columns or
// the leading key columns of an index. The evaluator applies every filter again
// to the rows it is given, so a filter left unused costs a wider read and never
// a wrong result.
class KeyFilteredTableIterator : public googlesql::EvaluatorTableIterator {
 public:
  // 'column_idxs' holds the table column index of each column of 'read_arg'.
  // 'dml_values', if set, narrows the read the same way as offered filters:
  // statements that modify rows do not offer their WHERE clause.
  KeyFilteredTableIterator(const QueryableTable* table, RowReader* reader,
                           ReadArg read_arg,
                           std::vector<const googlesql::Type*> column_types,
                           std::vector<int> column_idxs,
                           std::optional<ColumnValues> dml_values)
      : table_(table),
        reader_(reader),
        read_arg_(std::move(read_arg)),
        column_types_(std::move(column_types)),
        column_idxs_(std::move(column_idxs)),
        dml_values_(std::move(dml_values)) {}

  int NumColumns() const override {
    return static_cast<int>(read_arg_.columns.size());
  }

  std::string GetColumnName(int i) const override {
    return read_arg_.columns[i];
  }

  const googlesql::Type* GetColumnType(int i) const override {
    return column_types_[i];
  }

  absl::Status SetColumnFilterMap(
      absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>
          filter_map) override {
    if (rows_ != nullptr) {
      return absl::OkStatus();
    }
    ColumnValues values;
    for (const auto& [position, filter] : filter_map) {
      if (filter->kind() == googlesql::ColumnFilter::kInList) {
        values[column_idxs_[position]] = filter->in_list();
      }
    }
    Narrow(values);
    return absl::OkStatus();
  }

  bool NextRow() override {
    if (rows_ == nullptr) {
      if (dml_values_.has_value()) {
        Narrow(*dml_values_);
      }
      std::unique_ptr<RowCursor> cursor;
      read_status_ = reader_->Read(read_arg_, &cursor);
      if (!read_status_.ok()) {
        return false;
      }
      rows_ =
          std::make_unique<RowCursorEvaluatorTableIterator>(std::move(cursor));
    }
    return rows_->NextRow();
  }

  const googlesql::Value& GetValue(int i) const override {
    return rows_->GetValue(i);
  }

  absl::Status Status() const override {
    return rows_ == nullptr ? read_status_ : rows_->Status();
  }

  // Cancel is best-effort and not required.
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  void Narrow(const ColumnValues& values) {
    if (values.empty()) {
      return;
    }
    // A failed index read leaves the read whole; the evaluator still filters.
    absl::StatusOr<std::optional<KeySet>> key_set =
        table_->KeySetForColumnValues(values);
    if (key_set.ok() && key_set->has_value()) {
      read_arg_.key_set = **key_set;
    }
  }

  const QueryableTable* table_;

  RowReader* reader_;

  // What is read, its key set narrowed by SetColumnFilterMap.
  ReadArg read_arg_;

  std::vector<const googlesql::Type*> column_types_;

  std::vector<int> column_idxs_;

  std::optional<ColumnValues> dml_values_;

  // The rows, once the read has been made.
  std::unique_ptr<RowCursorEvaluatorTableIterator> rows_;

  absl::Status read_status_;
};

namespace {

// Whether SQL equality of the type is the equality of its key values.
bool HasKeyEquality(const googlesql::Type* type) {
  return type->IsInt64() || type->IsString() || type->IsBytes() ||
         type->IsBool() || type->IsDate() || type->IsTimestamp();
}

// Extends 'prefixes' by one key column with the values 'values' holds for it.
// Returns false, leaving 'prefixes' as they were, if the column has no values,
// a value is not of the column's type, or the keys would pass the limit.
bool ExtendPrefixes(const ColumnValues& values, int column_idx,
                    const googlesql::Type* column_type,
                    std::vector<Key>* prefixes) {
  auto wanted = values.find(column_idx);
  if (wanted == values.end() || !HasKeyEquality(column_type) ||
      !std::all_of(wanted->second.begin(), wanted->second.end(),
                   [column_type](const googlesql::Value& value) {
                     return value.type()->Equals(column_type);
                   }) ||
      prefixes->size() * wanted->second.size() > kMaxKeysFromFilters) {
    return false;
  }
  std::vector<Key> extended;
  extended.reserve(prefixes->size() * wanted->second.size());
  for (const Key& prefix : *prefixes) {
    for (const googlesql::Value& value : wanted->second) {
      Key key = prefix;
      key.AddColumn(value);
      extended.push_back(std::move(key));
    }
  }
  *prefixes = std::move(extended);
  return true;
}

KeySet PrefixKeySet(const std::vector<Key>& prefixes) {
  // An empty list is a filter no row passes, and leaves an empty key set.
  KeySet key_set;
  for (const Key& prefix : prefixes) {
    key_set.AddRange(KeyRange::Prefix(prefix));
  }
  return key_set;
}

}  // namespace

absl::StatusOr<std::optional<KeySet>> QueryableTable::KeySetForColumnValues(
    const ColumnValues& values) const {
  // The primary key first: it needs no second read.
  std::vector<Key> prefixes = {Key()};
  int narrowed = 0;
  for (int column_idx : primary_key_column_indexes_) {
    if (!ExtendPrefixes(values, column_idx, GetColumn(column_idx)->GetType(),
                        &prefixes)) {
      break;
    }
    ++narrowed;
  }
  if (narrowed > 0) {
    return PrefixKeySet(prefixes);
  }

  // Then the index with the most leading key columns the values narrow.
  const Index* best_index = nullptr;
  std::vector<Key> best_prefixes;
  int best_narrowed = 0;
  for (const Index* index : wrapped_table_->indexes()) {
    if (index->is_search_index() || index->is_vector_index()) {
      continue;
    }
    std::vector<Key> index_prefixes = {Key()};
    int index_narrowed = 0;
    for (const KeyColumn* key_column : index->key_columns()) {
      // Index key columns belong to the index data table; each has the column
      // of this table it copies as its source.
      const Column* source = key_column->column()->source_column();
      int column_idx = source == nullptr ? -1 : ColumnIndex(source);
      if (column_idx < 0 ||
          !ExtendPrefixes(values, column_idx,
                          GetColumn(column_idx)->GetType(), &index_prefixes)) {
        break;
      }
      ++index_narrowed;
    }
    if (index_narrowed > best_narrowed) {
      best_index = index;
      best_prefixes = std::move(index_prefixes);
      best_narrowed = index_narrowed;
    }
  }
  if (best_index == nullptr) {
    return std::nullopt;
  }

  ReadArg index_read;
  index_read.table = wrapped_table_->Name();
  index_read.index = best_index->Name();
  index_read.key_set = PrefixKeySet(best_prefixes);
  index_read.allow_pending_commit_timestamps = true;
  for (const KeyColumn* key_column : wrapped_table_->primary_key()) {
    index_read.columns.push_back(key_column->column()->Name());
  }
  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_RETURN_IF_ERROR(reader_->Read(index_read, &cursor));
  KeySet key_set;
  size_t keys = 0;
  while (cursor->Next()) {
    if (++keys > kMaxKeysFromFilters) {
      return std::nullopt;
    }
    Key key;
    for (int i = 0; i < cursor->NumColumns(); ++i) {
      key.AddColumn(cursor->ColumnValue(i));
    }
    key_set.AddKey(key);
  }
  GOOGLESQL_RETURN_IF_ERROR(cursor->Status());
  return key_set;
}

int QueryableTable::ColumnIndex(const Column* column) const {
  for (int i = 0; i < columns_.size(); ++i) {
    if (columns_[i]->wrapped_column() == column) {
      return i;
    }
  }
  return -1;
}

void QueryableTable::SetDmlColumnValues(ColumnValues values) const {
  dml_values_ = std::move(values);
}

absl::StatusOr<std::shared_ptr<const googlesql::AnalyzerOutput>>
QueryableTable::AnalyzeColumnExpression(
    const Column* column, googlesql::TypeFactory* type_factory,
    googlesql::Catalog* catalog,
    const std::optional<const googlesql::AnalyzerOptions>& opt_options,
    ColumnExpressionAnalysisCache* analyses) const {
  std::shared_ptr<const googlesql::AnalyzerOutput> output = nullptr;
  bool enable_generated_pk =
      EmulatorFeatureFlags::instance().flags().enable_generated_pk;
  bool is_generated_column = enable_generated_pk && column->is_generated();
  if (opt_options.has_value() &&
      (column->has_default_value() || (is_generated_column))) {
    std::string key;
    if (analyses != nullptr) {
      key = ColumnExpressionAnalysisCache::KeyFor(column, opt_options.value());
      output = analyses->Find(key);
      if (output != nullptr) {
        return output;
      }
    }
    googlesql::AnalyzerOptions options = opt_options.value();
    if (is_generated_column) {
      for (const Column* dep : column->dependent_columns()) {
        GOOGLESQL_RETURN_IF_ERROR(
            options.AddExpressionColumn(dep->Name(), dep->GetType()))
            << "Failed to add dependent column " << dep->Name()
            << " for generated column : " << column->FullName();
      }
    }
    std::string expression_type = "default";
    if (is_generated_column) {
      expression_type = "generated";
    }
    std::unique_ptr<const googlesql::AnalyzerOutput> analyzed;
    GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeExpressionForAssignmentToType(
        column->expression().value(), options, catalog, type_factory,
        column->GetType(), &analyzed))
        << "Failed to analyze " << expression_type << " expression for column "
        << column->FullName();
    output = std::move(analyzed);
    if (analyses != nullptr) {
      analyses->Store(key, output);
    }
  }
  return output;
}

QueryableTable::QueryableTable(
    const backend::Table* table, RowReader* reader,
    const std::optional<const googlesql::AnalyzerOptions>& opt_options,
    googlesql::Catalog* catalog, googlesql::TypeFactory* type_factory,
    bool is_synonym, ColumnExpressionAnalysisCache* analyses)
    : is_synonym_(is_synonym), wrapped_table_(table), reader_(reader) {
  bool enable_generated_pk =
      EmulatorFeatureFlags::instance().flags().enable_generated_pk;
  for (const auto* column : table->columns()) {
    absl::StatusOr<std::shared_ptr<const googlesql::AnalyzerOutput>>
        analyzer_output =
            AnalyzeColumnExpression(column, type_factory, catalog, opt_options,
                                    analyses);
    ABSL_CHECK_OK(analyzer_output.status());  // Crash OK
    std::shared_ptr<const googlesql::AnalyzerOutput> output =
        std::move(analyzer_output.value());
    bool is_generated_column = enable_generated_pk && column->is_generated();
    if (column->has_default_value() || (is_generated_column)) {
      googlesql::Column::ExpressionAttributes::ExpressionKind expression_kind =
          googlesql::Column::ExpressionAttributes::ExpressionKind::DEFAULT;
      if (is_generated_column) {
        expression_kind =
            googlesql::Column::ExpressionAttributes::ExpressionKind::GENERATED;
      }
      googlesql::Column::ExpressionAttributes expression_attributes =
          googlesql::Column::ExpressionAttributes(expression_kind,
                                                  column->expression().value(),
                                                  output->resolved_expr());
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, std::move(output),
          std::make_optional(expression_attributes)));
    } else {
      columns_.push_back(std::make_unique<const QueryableColumn>(
          column, std::move(output), std::nullopt));
    }
  }

  // Populate primary_key_column_indexes_.
  for (const auto& key_column : table->primary_key()) {
    for (int i = 0; i < wrapped_table_->columns().size(); ++i) {
      if (key_column->column() == wrapped_table_->columns()[i]) {
        primary_key_column_indexes_.push_back(i);
        break;
      }
    }
  }
}

absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
QueryableTable::CreateEvaluatorTableIterator(
    absl::Span<const int> column_idxs) const {
  GOOGLESQL_RET_CHECK_NE(reader_, nullptr);

  std::vector<std::string> column_names;
  for (int idx : column_idxs) {
    column_names.push_back(GetColumn(idx)->Name());
  }

  ReadArg read_arg;
  read_arg.table = FullName();
  read_arg.key_set = KeySet::All();
  read_arg.columns = column_names;
  // Pending commit timestamp restrictions for queries are implemented in
  // QueryValidator so we do not need enforcement during the read here.
  // Furthermore, without enabling this certain internal reads issued by the
  // GoogleSQL reference implementation will be rejected.
  read_arg.allow_pending_commit_timestamps = true;

  // If current table is a change stream internal data/partition table, change
  // the read arg to access internal tables directly.
  if (wrapped_table_->owner_change_stream() != nullptr) {
    absl::string_view change_stream_name = read_arg.table;
    if (absl::StartsWith(read_arg.table, kChangeStreamPartitionTablePrefix)) {
      absl::ConsumePrefix(&change_stream_name,
                          kChangeStreamPartitionTablePrefix);
      read_arg.change_stream_for_partition_table = change_stream_name;
    } else {
      absl::ConsumePrefix(&change_stream_name, kChangeStreamDataTablePrefix);
      read_arg.change_stream_for_data_table = change_stream_name;
    }
  }

  // The values a modifying statement allows, for its one read of the table.
  std::optional<ColumnValues> dml_values = std::move(dml_values_);
  dml_values_.reset();

  if (!KeyFilterPushdownEnabled() ||
      wrapped_table_->owner_change_stream() != nullptr ||
      primary_key_column_indexes_.size() !=
          wrapped_table_->primary_key().size()) {
    std::unique_ptr<RowCursor> cursor;
    GOOGLESQL_RETURN_IF_ERROR(reader_->Read(read_arg, &cursor));
    return std::make_unique<RowCursorEvaluatorTableIterator>(
        std::move(cursor));
  }

  std::vector<const googlesql::Type*> column_types;
  column_types.reserve(column_idxs.size());
  for (int idx : column_idxs) {
    column_types.push_back(GetColumn(idx)->GetType());
  }
  return std::make_unique<KeyFilteredTableIterator>(
      this, reader_, std::move(read_arg), std::move(column_types),
      std::vector<int>(column_idxs.begin(), column_idxs.end()),
      std::move(dml_values));
}

const googlesql::Column* QueryableTable::FindColumnByName(
    const std::string& name) const {
  const auto* to_find = wrapped_table_->FindColumn(name);
  auto it = std::find_if(columns_.begin(), columns_.end(),
                         [to_find](const auto& column) {
                           return column->wrapped_column() == to_find;
                         });
  if (it == columns_.end()) {
    return nullptr;
  }
  return it->get();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
