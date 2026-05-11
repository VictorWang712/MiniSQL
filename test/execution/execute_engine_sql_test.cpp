#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "common/instance.h"
#include "executor/execute_engine.h"
#include "gtest/gtest.h"

extern "C" {
#include "parser/minisql_lex.h"
#include "parser/parser.h"
int yyparse(void);
}

namespace {

dberr_t ExecuteSql(ExecuteEngine &engine, const std::string &sql) {
  YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
  if (buffer == nullptr) {
    return DB_FAILED;
  }
  yy_switch_to_buffer(buffer);
  MinisqlParserInit();
  yyparse();

  dberr_t result = DB_FAILED;
  if (!MinisqlParserGetError()) {
    result = engine.Execute(MinisqlGetParserRootNode());
  }

  MinisqlParserFinish();
  yy_delete_buffer(buffer);
  yylex_destroy();
  return result;
}

std::vector<std::string> CollectIndexNames(CatalogManager *catalog, const std::string &table_name) {
  std::vector<IndexInfo *> indexes;
  EXPECT_EQ(DB_SUCCESS, catalog->GetTableIndexes(table_name, indexes));
  std::vector<std::string> index_names;
  index_names.reserve(indexes.size());
  for (auto *index : indexes) {
    index_names.push_back(index->GetIndexName());
  }
  return index_names;
}

}  // namespace

class ExecuteEngineSqlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    db_name_ = "planner_executor_sql_test";
    execfile_db_name_ = "planner_executor_execfile_test";
    execfile_path_ = "planner_executor_execfile_sql";
    std::remove(("./databases/" + db_name_).c_str());
    std::remove(("./databases/" + execfile_db_name_).c_str());
    std::remove(execfile_path_.c_str());
  }

  void TearDown() override {
    std::remove(("./databases/" + db_name_).c_str());
    std::remove(("./databases/" + execfile_db_name_).c_str());
    std::remove(execfile_path_.c_str());
  }

  std::string db_name_;
  std::string execfile_db_name_;
  std::string execfile_path_;
};

TEST_F(ExecuteEngineSqlTest, CreateTableBuildsConstraintIndexes) {
  {
    ExecuteEngine engine;
    ASSERT_EQ(DB_SUCCESS, ExecuteSql(engine, "create database planner_executor_sql_test;"));
    ASSERT_EQ(DB_SUCCESS, ExecuteSql(engine, "use planner_executor_sql_test;"));
    ASSERT_EQ(DB_SUCCESS, ExecuteSql(engine,
                                     "create table account(id int, email char(16) unique, balance float, "
                                     "primary key(id, balance));"));
    ASSERT_EQ(DB_FAILED, ExecuteSql(engine, "create table invalid_code(code char(0));"));

    testing::internal::CaptureStdout();
    ASSERT_EQ(DB_SUCCESS, ExecuteSql(engine, "show indexes;"));
    std::string output = testing::internal::GetCapturedStdout();
    ASSERT_NE(output.find("__uniq__account_email"), std::string::npos);
    ASSERT_NE(output.find("__pk__account"), std::string::npos);
  }

  DBStorageEngine storage(db_name_, false);
  TableInfo *table_info = nullptr;
  ASSERT_EQ(DB_SUCCESS, storage.catalog_mgr_->GetTable("account", table_info));
  ASSERT_NE(table_info, nullptr);

  TableInfo *invalid_table = nullptr;
  ASSERT_EQ(DB_TABLE_NOT_EXIST, storage.catalog_mgr_->GetTable("invalid_code", invalid_table));

  auto index_names = CollectIndexNames(storage.catalog_mgr_, "account");
  ASSERT_EQ(index_names.size(), 2);
  ASSERT_NE(std::find(index_names.begin(), index_names.end(), "__uniq__account_email"), index_names.end());
  ASSERT_NE(std::find(index_names.begin(), index_names.end(), "__pk__account"), index_names.end());
}

TEST_F(ExecuteEngineSqlTest, ExecfileExecutesUtilityAndIndexStatements) {
  std::ofstream ofs(execfile_path_);
  ofs << "create database " << execfile_db_name_ << ";\n";
  ofs << "use " << execfile_db_name_ << ";\n";
  ofs << "create table t1(id int, name char(8) unique, score float, primary key(id));\n";
  ofs << "create table t2(id int, nick char(8), score float);\n";
  ofs << "create index idx_manual on t2(id) using btree;\n";
  ofs << "drop index idx_manual;\n";
  ofs.close();

  {
    ExecuteEngine engine;
    ASSERT_EQ(DB_SUCCESS, ExecuteSql(engine, "execfile \"planner_executor_execfile_sql\";"));
    ASSERT_EQ(DB_QUIT, ExecuteSql(engine, "quit;"));
  }

  DBStorageEngine storage(execfile_db_name_, false);
  TableInfo *t1 = nullptr;
  TableInfo *t2 = nullptr;
  ASSERT_EQ(DB_SUCCESS, storage.catalog_mgr_->GetTable("t1", t1));
  ASSERT_EQ(DB_SUCCESS, storage.catalog_mgr_->GetTable("t2", t2));
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);

  auto t1_indexes = CollectIndexNames(storage.catalog_mgr_, "t1");
  ASSERT_EQ(t1_indexes.size(), 2);
  ASSERT_NE(std::find(t1_indexes.begin(), t1_indexes.end(), "__uniq__t1_name"), t1_indexes.end());
  ASSERT_NE(std::find(t1_indexes.begin(), t1_indexes.end(), "__pk__t1"), t1_indexes.end());

  std::vector<IndexInfo *> t2_indexes;
  ASSERT_EQ(DB_SUCCESS, storage.catalog_mgr_->GetTableIndexes("t2", t2_indexes));
  ASSERT_TRUE(t2_indexes.empty());
}
