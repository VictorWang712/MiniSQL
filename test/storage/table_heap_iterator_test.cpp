#include "storage/table_heap.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"

TEST(TableHeapIteratorTest, IterateUpdateAndDelete) {
  const std::string db_name = "table_heap_iterator_test.db";
  remove(db_name.c_str());

  auto *disk_mgr = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(32, disk_mgr);
  std::vector<Column *> columns = {new Column("id", TypeId::kTypeInt, 0, false, false),
                                   new Column("name", TypeId::kTypeChar, 16, 1, true, false)};
  auto schema = std::make_shared<Schema>(columns);
  auto *table_heap = TableHeap::Create(bpm, schema.get(), nullptr, nullptr, nullptr);

  std::vector<RowId> rids;
  for (int i = 0; i < 5; i++) {
    std::string name = "n" + std::to_string(i);
    std::vector<Field> fields = {Field(TypeId::kTypeInt, i),
                                 Field(TypeId::kTypeChar, const_cast<char *>(name.data()), name.size(), true)};
    Row row(fields);
    ASSERT_TRUE(table_heap->InsertTuple(row, nullptr));
    rids.push_back(row.GetRowId());
  }

  int iterated = 0;
  int value_sum = 0;
  for (auto iter = table_heap->Begin(nullptr); iter != table_heap->End(); iter++) {
    iterated++;
    value_sum += iter->GetField(0)->CompareEquals(Field(TypeId::kTypeInt, iterated - 1)) == CmpBool::kTrue
                     ? iterated - 1
                     : 0;
  }
  EXPECT_EQ(5, iterated);
  EXPECT_EQ(10, value_sum);

  std::vector<Field> updated_fields = {Field(TypeId::kTypeInt, 100),
                                       Field(TypeId::kTypeChar, const_cast<char *>("zz"), 2, true)};
  Row updated_row(updated_fields);
  ASSERT_TRUE(table_heap->UpdateTuple(updated_row, rids[2], nullptr));

  Row fetched(rids[2]);
  ASSERT_TRUE(table_heap->GetTuple(&fetched, nullptr));
  EXPECT_EQ(CmpBool::kTrue, fetched.GetField(0)->CompareEquals(updated_fields[0]));
  EXPECT_EQ(CmpBool::kTrue, fetched.GetField(1)->CompareEquals(updated_fields[1]));

  ASSERT_TRUE(table_heap->MarkDelete(rids[1], nullptr));
  table_heap->ApplyDelete(rids[1], nullptr);
  Row deleted(rids[1]);
  EXPECT_FALSE(table_heap->GetTuple(&deleted, nullptr));

  int remaining = 0;
  for (auto iter = table_heap->Begin(nullptr); iter != table_heap->End(); ++iter) {
    remaining++;
  }
  EXPECT_EQ(4, remaining);

  table_heap->FreeTableHeap();
  disk_mgr->Close();
  remove(db_name.c_str());
  delete table_heap;
  delete bpm;
  delete disk_mgr;
}
