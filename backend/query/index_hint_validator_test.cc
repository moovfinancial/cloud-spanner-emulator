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

#include <memory>
#include <string>

#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/types/type_factory.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/memory/memory.h"
#include "backend/query/analyzer_options.h"
#include "backend/query/catalog.h"
#include "backend/query/function_catalog.h"
#include "backend/query/queryable_table.h"
#include "backend/schema/catalog/schema.h"
#include "common/constants.h"
#include "common/errors.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

class IndexHintValidatorTest : public testing::Test {
 public:
  IndexHintValidatorTest()
      : analyzer_options_(MakeGoogleSqlAnalyzerOptions(kDefaultTimeZone)) {}

  void SetUp() override {
    GOOGLESQL_ASSERT_OK(analyzer_options_.AddQueryParameter(
        "p", googlesql::types::StringType()));
    GOOGLESQL_ASSERT_OK(analyzer_options_.AddQueryParameter(
        "ps", googlesql::types::StringArrayType()));

    GOOGLESQL_ASSERT_OK_AND_ASSIGN(schema_, test::CreateSchemaFromDDL(
                                      {
                                          R"(
      CREATE TABLE T1 (
        k1 INT64,
        col1 STRING(MAX),
        gcol STRING(MAX) AS (IF(col1 = 'a', col1, NULL)) STORED
      ) PRIMARY KEY (k1))",
                                          R"(
      CREATE TABLE T2 (
        k2 INT64,
        col2 STRING(MAX)
      ) PRIMARY KEY (k2))",
                                          R"(
      CREATE TABLE T3 (
        k3 INT64 NOT NULL,
        col3 STRING(MAX) NOT NULL
      ) PRIMARY KEY (k3))",
                                          R"(
      CREATE INDEX I1 ON T1(col1))",
                                          R"(
      CREATE INDEX I2 ON T2(col2))",
                                          R"(
      CREATE NULL_FILTERED INDEX NF_I1 ON T1(col1))",
                                          R"(
      CREATE NULL_FILTERED INDEX NF_G ON T1(gcol))",
                                          R"(
      CREATE NULL_FILTERED INDEX I3 ON T3(col3))",
                                          R"(
      ALTER TABLE T2 ADD FOREIGN KEY(col2) REFERENCES T1(col1))"},
                                      &type_factory_));

    fn_catalog_ = std::make_unique<FunctionCatalog>(
        &type_factory_,
        /*catalog_name=*/kCloudSpannerEmulatorFunctionCatalogName,
        /*latest_schema=*/schema_.get());

    catalog_ = std::make_unique<Catalog>(schema_.get(), fn_catalog_.get(),
                                         &type_factory_, analyzer_options_);
  }

  std::unique_ptr<const googlesql::AnalyzerOutput> AnalyzeQuery(
      const std::string& sql) {
    std::unique_ptr<const googlesql::AnalyzerOutput> output;
    GOOGLESQL_EXPECT_OK(googlesql::AnalyzeStatement(
        sql, analyzer_options_, catalog_.get(), &type_factory_, &output));
    return output;
  }

  const Schema* schema() const { return schema_.get(); }

 private:
  googlesql::TypeFactory type_factory_;

  googlesql::AnalyzerOptions analyzer_options_;

  std::unique_ptr<const FunctionCatalog> fn_catalog_;

  std::unique_ptr<const Schema> schema_;

  std::unique_ptr<Catalog> catalog_;
};

TEST_F(IndexHintValidatorTest, ValidateTableIndexHint) {
  auto output = AnalyzeQuery("SELECT k1 FROM T1@{force_index=I1}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  GOOGLESQL_EXPECT_OK(stmt->Accept(&validator));
}

TEST_F(IndexHintValidatorTest, InvalidIndexHintReturnsError) {
  auto output = AnalyzeQuery("SELECT k1 FROM T1@{force_index=I2}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  EXPECT_EQ(stmt->Accept(&validator),
            error::QueryHintIndexNotFound("T1", "I2"));
}

TEST_F(IndexHintValidatorTest, InvalidIndexHintInInsertReturnsError) {
  auto output =
      AnalyzeQuery("INSERT INTO T1(k1) SELECT k1 FROM T1@{force_index=I2}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  EXPECT_EQ(stmt->Accept(&validator),
            error::QueryHintIndexNotFound("T1", "I2"));
}

TEST_F(IndexHintValidatorTest, InvalidIndexHintInUpdateReturnsError) {
  auto output = AnalyzeQuery(
      "UPDATE T1 SET k1 = 1 WHERE k1 IN "
      "(SELECT k1 FROM T1@{force_index=I2})");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  EXPECT_EQ(stmt->Accept(&validator),
            error::QueryHintIndexNotFound("T1", "I2"));
}

TEST_F(IndexHintValidatorTest, InvalidIndexHintInDeleteReturnsError) {
  auto output = AnalyzeQuery(
      "DELETE FROM T1 WHERE k1 IN "
      "(SELECT k1 FROM T1@{force_index=I2})");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  EXPECT_EQ(stmt->Accept(&validator),
            error::QueryHintIndexNotFound("T1", "I2"));
}

TEST_F(IndexHintValidatorTest, ValidIndexHintOnDeleteTargetSucceeds) {
  auto output =
      AnalyzeQuery("DELETE FROM T1@{force_index=I1} WHERE col1 = 'value'");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  GOOGLESQL_EXPECT_OK(stmt->Accept(&validator));
}

TEST_F(IndexHintValidatorTest,
       NullFilteredIndexCannotBeUsedForNullableColumns) {
  auto output = AnalyzeQuery("SELECT col1 FROM T1@{force_index=NF_I1}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  EXPECT_EQ(stmt->Accept(&validator),
            error::NullFilteredIndexUnusable("NF_I1"));
}

TEST_F(IndexHintValidatorTest, DisableNullFilteredIndexCheck) {
  // Cloud Spanner accepts `UPPER(col1) = 'A'` as a filter that rejects NULLs,
  // but the emulator does not inspect function calls on the column. This is
  // an emulator limitation, so the hint bypasses it.
  auto stmt1 = AnalyzeQuery(
      "SELECT col1 FROM T1@{force_index=NF_I1} "
      "WHERE UPPER(col1) = 'A'");
  // Statement is rejected without the disabling index hint.
  {
    IndexHintValidator validator{schema()};
    EXPECT_EQ(stmt1->resolved_statement()->Accept(&validator),
              error::NullFilteredIndexUnusable("NF_I1"));
  }

  // Statement is accepted after disabling the check through a hint.
  auto stmt2 = AnalyzeQuery(
      "SELECT col1 FROM T1"
      "@{force_index=NF_I1, "
      "spanner_emulator.disable_query_null_filtered_index_check=true} "
      "WHERE UPPER(col1) = 'A'");
  {
    IndexHintValidator validator{schema(),
                                 /*disable_null_filtered_index_check=*/true};
    GOOGLESQL_EXPECT_OK(stmt2->resolved_statement()->Accept(&validator));
  }
}

TEST_F(IndexHintValidatorTest, NullFilteredIndexWithIsNotNull) {
  for (const char* sql : {
           "SELECT col1 FROM T1@{force_index=NF_I1} WHERE col1 IS NOT NULL",
           "SELECT k1 FROM (SELECT k1, col1 FROM T1@{force_index=NF_I1}) "
           "WHERE col1 IS NOT NULL",
           "SELECT k2 FROM T2 LEFT JOIN T1@{force_index=NF_I1} ON k1 = k2 "
           "WHERE col1 IS NOT NULL",
       }) {
    SCOPED_TRACE(sql);
    auto output = AnalyzeQuery(sql);
    IndexHintValidator validator{schema()};
    GOOGLESQL_EXPECT_OK(output->resolved_statement()->Accept(&validator));
  }
}

// Each form was accepted by Cloud Spanner in PLAN mode for a forced
// NULL_FILTERED index.
TEST_F(IndexHintValidatorTest, NullFilteredIndexWithComparison) {
  for (const char* sql : {
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 = @p",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE @p = col1",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 = 'a'",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE k1 > 0 AND col1 = @p",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 = @p AND col1 IN UNNEST(@ps)",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 != @p",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 < @p",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 <= 'a'",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 > UPPER(@p)",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE @p >= col1",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 = CAST(k1 AS "
           "STRING)",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 BETWEEN @p AND "
           "'z'",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 IN ('a', 'b')",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 IN (@p, 'a')",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 > @p AND col1 IN UNNEST(@ps)",
           "SELECT k1 FROM T1@{force_index=NF_G} WHERE gcol = @p",
           "SELECT k1 FROM T1@{force_index=NF_G} WHERE gcol IN ('a', 'b')",
           "DELETE FROM T1@{force_index=NF_I1} WHERE col1 < @p",
       }) {
    SCOPED_TRACE(sql);
    auto output = AnalyzeQuery(sql);
    IndexHintValidator validator{schema()};
    GOOGLESQL_EXPECT_OK(output->resolved_statement()->Accept(&validator));
  }
}

TEST_F(IndexHintValidatorTest, NullFilteredIndexWithUnprovenFilter) {
  for (const char* sql : {
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE k1 = 1",
           "SELECT k1 FROM (SELECT k1, UPPER(col1) AS col1 "
           "FROM T1@{force_index=NF_I1}) WHERE col1 IS NOT NULL",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IS NOT NULL OR k1 = 1",
           // Cloud Spanner rejects these forms.
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 IS NULL",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IS NOT DISTINCT FROM @p",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE IFNULL(col1, @p) = @p",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE (col1 = @p) IS TRUE",
           // Cloud Spanner accepts these forms, but the emulator does not
           // inspect them. The hint bypasses the check.
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE UPPER(col1) = 'A'",
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE NOT (col1 = @p)",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IN UNNEST(['a', 'b'])",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IN UNNEST([@p, 'a'])",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IN UNNEST(ARRAY(SELECT col2 FROM T2))",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IN UNNEST(IFNULL(@ps, ['a']))",
       }) {
    SCOPED_TRACE(sql);
    auto output = AnalyzeQuery(sql);
    IndexHintValidator validator{schema()};
    EXPECT_EQ(output->resolved_statement()->Accept(&validator),
              error::NullFilteredIndexUnusable("NF_I1"));
  }
}

TEST_F(IndexHintValidatorTest, NullFilteredIndexWithInUnnest) {
  for (const char* sql : {
           "SELECT k1 FROM T1@{force_index=NF_I1} WHERE col1 IN UNNEST(@ps)",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE k1 > 0 AND col1 IN UNNEST(@ps)",
           "SELECT k1 FROM T1@{force_index=NF_I1} "
           "WHERE col1 IN UNNEST(CAST(@ps AS ARRAY<STRING>))",
           // The other scan is only unproven. The IN UNNEST error wins.
           "SELECT a.k1 FROM T1@{force_index=NF_I1} a "
           "JOIN T1@{force_index=NF_I1} b ON a.k1 = b.k1 "
           "WHERE b.col1 IN UNNEST(@ps)",
           "SELECT a.k1 FROM T1@{force_index=NF_I1} a "
           "JOIN T1@{force_index=NF_I1} b ON a.k1 = b.k1 "
           "WHERE a.col1 IN UNNEST(@ps)",
           "SELECT k2 FROM T2 JOIN T1@{force_index=NF_I1} ON k1 = k2 "
           "WHERE col1 IN UNNEST(@ps)",
           "SELECT k2 FROM T2 LEFT JOIN T1@{force_index=NF_I1} ON k1 = k2 "
           "WHERE col1 IN UNNEST(@ps)",
           "SELECT k1 FROM (SELECT k1, col1 FROM T1@{force_index=NF_I1}) "
           "WHERE col1 IN UNNEST(@ps)",
           "DELETE FROM T1@{force_index=NF_I1} WHERE col1 IN UNNEST(@ps)",
           "UPDATE T1@{force_index=NF_I1} SET col1 = 'x' "
           "WHERE col1 IN UNNEST(@ps)",
       }) {
    SCOPED_TRACE(sql);
    auto output = AnalyzeQuery(sql);
    // The hint to disable the null-filtered index check does not apply,
    // because Cloud Spanner rejects these queries.
    for (bool disable : {false, true}) {
      IndexHintValidator validator{schema(),
                                   /*disable_null_filtered_index_check=*/
                                   disable};
      EXPECT_EQ(output->resolved_statement()->Accept(&validator),
                error::NullFilteredIndexUnusableForInUnnest("NF_I1", "col1"));
    }
  }
}

TEST_F(IndexHintValidatorTest, NullFilteredIndexWithInUnnestAndIsNotNull) {
  auto output = AnalyzeQuery(
      "SELECT k1 FROM T1@{force_index=NF_I1} "
      "WHERE col1 IS NOT NULL AND col1 IN UNNEST(@ps)");
  IndexHintValidator validator{schema()};
  GOOGLESQL_EXPECT_OK(output->resolved_statement()->Accept(&validator));
}

TEST_F(IndexHintValidatorTest, InUnnestOnIndexWithoutNullFiltering) {
  auto output = AnalyzeQuery(
      "SELECT k1 FROM T1@{force_index=I1} WHERE col1 IN UNNEST(@ps)");
  IndexHintValidator validator{schema()};
  GOOGLESQL_EXPECT_OK(output->resolved_statement()->Accept(&validator));
}

TEST_F(IndexHintValidatorTest,
       NullFilteredIndexCanOnlyBeUsedForNotNullColumns) {
  auto output = AnalyzeQuery("SELECT col3 FROM T3@{force_index=I3}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  GOOGLESQL_EXPECT_OK(stmt->Accept(&validator));
}

TEST_F(IndexHintValidatorTest, EmulatorManagedIndexName) {
  auto output = AnalyzeQuery(
      "SELECT k1 FROM T1@{force_index=IDX_T1_col1_U_EA26CF5871E82344}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  EXPECT_EQ(stmt->Accept(&validator), error::QueryHintManagedIndexNotSupported(
                                          "IDX_T1_col1_U_EA26CF5871E82344"));
}

TEST_F(IndexHintValidatorTest, NonEmulatorManagedIndexName) {
  auto output = AnalyzeQuery(
      "SELECT k1 FROM T1@{force_index=IDX_T1_col1_U_EA26CF5871E82340}");
  auto stmt = output->resolved_statement();
  IndexHintValidator validator{schema()};
  GOOGLESQL_EXPECT_OK(stmt->Accept(&validator));
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
