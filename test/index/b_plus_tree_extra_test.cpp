#include "common/instance.h"
#include "gtest/gtest.h"
#include "index/b_plus_tree.h"

static const std::string db_name_extra = "bp_tree_extra_test.db";

TEST(BPlusTreeTests, DuplicateInsertRangeBeginAndRootShrinkTest) {
  DBStorageEngine engine(db_name_extra);
  std::vector<Column *> columns = {new Column("int", TypeId::kTypeInt, 0, false, true)};
  Schema *table_schema = new Schema(columns);
  KeyManager key_manager(table_schema, 16);
  BPlusTree tree(1, engine.bpm_, key_manager);

  std::vector<GenericKey *> keys;
  for (int i = 1; i <= 20; i++) {
    GenericKey *key = key_manager.InitKey();
    std::vector<Field> fields{Field(TypeId::kTypeInt, i)};
    Row row(fields);
    key_manager.SerializeFromKey(key, row, table_schema);
    ASSERT_TRUE(tree.Insert(key, RowId(2000, i), nullptr));
    keys.push_back(key);
  }

  GenericKey *dup = key_manager.InitKey();
  std::vector<Field> dup_fields{Field(TypeId::kTypeInt, 10)};
  Row dup_row(dup_fields);
  key_manager.SerializeFromKey(dup, dup_row, table_schema);
  ASSERT_FALSE(tree.Insert(dup, RowId(9999, 10), nullptr));

  GenericKey *low_key = key_manager.InitKey();
  std::vector<Field> low_fields{Field(TypeId::kTypeInt, 8)};
  Row low_row(low_fields);
  key_manager.SerializeFromKey(low_key, low_row, table_schema);
  auto iter = tree.Begin(low_key);
  ASSERT_NE(iter, tree.End());
  EXPECT_EQ(RowId(2000, 8), (*iter).second);
  ++iter;
  EXPECT_EQ(RowId(2000, 9), (*iter).second);

  for (auto key : keys) {
    tree.Remove(key, nullptr);
  }
  std::vector<RowId> result;
  EXPECT_FALSE(tree.GetValue(low_key, result, nullptr));
  EXPECT_EQ(tree.Begin(), tree.End());

  free(dup);
  free(low_key);
  delete table_schema;
}
