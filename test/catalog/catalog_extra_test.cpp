#include "catalog/catalog.h"

#include "common/instance.h"
#include "gtest/gtest.h"

static std::string extra_db_file_name = "catalog_extra_test.db";

TEST(CatalogTest, DropAndListTest) {
  auto *db = new DBStorageEngine(extra_db_file_name, true);
  auto &catalog = db->catalog_mgr_;

  std::vector<Column *> columns = {new Column("id", TypeId::kTypeInt, 0, false, true),
                                   new Column("name", TypeId::kTypeChar, 32, 1, true, false)};
  auto schema = std::make_shared<Schema>(columns);
  Txn txn;

  TableInfo *table_info = nullptr;
  ASSERT_EQ(DB_SUCCESS, catalog->CreateTable("users", schema.get(), &txn, table_info));

  std::vector<TableInfo *> tables;
  ASSERT_EQ(DB_SUCCESS, catalog->GetTables(tables));
  ASSERT_EQ(1, tables.size());
  ASSERT_EQ("users", tables[0]->GetTableName());

  IndexInfo *index_info = nullptr;
  ASSERT_EQ(DB_SUCCESS, catalog->CreateIndex("users", "idx_users_id", {"id"}, &txn, index_info, "bptree"));

  std::vector<IndexInfo *> indexes;
  ASSERT_EQ(DB_SUCCESS, catalog->GetTableIndexes("users", indexes));
  ASSERT_EQ(1, indexes.size());
  ASSERT_EQ("idx_users_id", indexes[0]->GetIndexName());

  ASSERT_EQ(DB_SUCCESS, catalog->DropIndex("users", "idx_users_id"));
  indexes.clear();
  ASSERT_EQ(DB_SUCCESS, catalog->GetTableIndexes("users", indexes));
  ASSERT_TRUE(indexes.empty());
  ASSERT_EQ(DB_INDEX_NOT_FOUND, catalog->DropIndex("users", "idx_users_id"));

  ASSERT_EQ(DB_SUCCESS, catalog->DropTable("users"));
  TableInfo *loaded = nullptr;
  ASSERT_EQ(DB_TABLE_NOT_EXIST, catalog->GetTable("users", loaded));
  ASSERT_EQ(DB_TABLE_NOT_EXIST, catalog->DropTable("users"));

  delete db;
}
