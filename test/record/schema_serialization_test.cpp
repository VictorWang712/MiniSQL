#include <memory>

#include "gtest/gtest.h"
#include "record/row.h"
#include "record/schema.h"

TEST(SchemaSerializationTest, ColumnAndSchemaRoundTrip) {
  std::vector<Column *> columns = {new Column("id", TypeId::kTypeInt, 0, false, true),
                                   new Column("name", TypeId::kTypeChar, 32, 1, true, false),
                                   new Column("score", TypeId::kTypeFloat, 2, true, false)};
  Schema schema(columns);

  char buffer[PAGE_SIZE];
  memset(buffer, 0, sizeof(buffer));
  uint32_t bytes = schema.SerializeTo(buffer);
  EXPECT_EQ(schema.GetSerializedSize(), bytes);

  Schema *deserialized = nullptr;
  uint32_t read_bytes = Schema::DeserializeFrom(buffer, deserialized);
  ASSERT_NE(nullptr, deserialized);
  EXPECT_EQ(bytes, read_bytes);
  EXPECT_EQ(schema.GetColumnCount(), deserialized->GetColumnCount());

  for (uint32_t i = 0; i < schema.GetColumnCount(); i++) {
    auto lhs = schema.GetColumn(i);
    auto rhs = deserialized->GetColumn(i);
    EXPECT_EQ(lhs->GetName(), rhs->GetName());
    EXPECT_EQ(lhs->GetType(), rhs->GetType());
    EXPECT_EQ(lhs->GetLength(), rhs->GetLength());
    EXPECT_EQ(lhs->GetTableInd(), rhs->GetTableInd());
    EXPECT_EQ(lhs->IsNullable(), rhs->IsNullable());
    EXPECT_EQ(lhs->IsUnique(), rhs->IsUnique());
  }

  delete deserialized;
}

TEST(SchemaSerializationTest, RowRoundTripWithNullBitmap) {
  std::vector<Column *> columns = {new Column("id", TypeId::kTypeInt, 0, false, false),
                                   new Column("name", TypeId::kTypeChar, 16, 1, true, false),
                                   new Column("score", TypeId::kTypeFloat, 2, true, false)};
  Schema schema(columns);

  std::vector<Field> fields = {Field(TypeId::kTypeInt, 7), Field(TypeId::kTypeChar), Field(TypeId::kTypeFloat, 3.5f)};
  Row row(fields);

  char buffer[PAGE_SIZE];
  memset(buffer, 0, sizeof(buffer));
  uint32_t bytes = row.SerializeTo(buffer, &schema);
  EXPECT_EQ(row.GetSerializedSize(&schema), bytes);

  Row deserialized;
  uint32_t read_bytes = deserialized.DeserializeFrom(buffer, &schema);
  EXPECT_EQ(bytes, read_bytes);
  ASSERT_EQ(3, deserialized.GetFieldCount());
  EXPECT_EQ(CmpBool::kTrue, deserialized.GetField(0)->CompareEquals(fields[0]));
  EXPECT_TRUE(deserialized.GetField(1)->IsNull());
  EXPECT_EQ(CmpBool::kTrue, deserialized.GetField(2)->CompareEquals(fields[2]));
}
