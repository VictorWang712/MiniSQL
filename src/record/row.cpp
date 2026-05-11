#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
  if (fields_.empty()) {
    return 0;
  }

  char *pos = buf;
  uint32_t field_count = static_cast<uint32_t>(fields_.size());
  uint32_t bitmap_size = (field_count + 7) / 8;

  MACH_WRITE_UINT32(pos, field_count);
  pos += sizeof(uint32_t);
  memset(pos, 0, bitmap_size);
  char *bitmap = pos;
  pos += bitmap_size;

  for (uint32_t i = 0; i < field_count; i++) {
    if (fields_[i]->IsNull()) {
      bitmap[i / 8] |= static_cast<char>(1U << (i % 8));
      continue;
    }
    pos += fields_[i]->SerializeTo(pos);
  }
  return static_cast<uint32_t>(pos - buf);
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");
  char *pos = buf;
  uint32_t field_count = MACH_READ_UINT32(pos);
  pos += sizeof(uint32_t);
  ASSERT(field_count == schema->GetColumnCount(), "Field count does not match schema.");

  uint32_t bitmap_size = (field_count + 7) / 8;
  unsigned char *bitmap = reinterpret_cast<unsigned char *>(pos);
  pos += bitmap_size;

  fields_.reserve(field_count);
  for (uint32_t i = 0; i < field_count; i++) {
    bool is_null = (bitmap[i / 8] & static_cast<unsigned char>(1U << (i % 8))) != 0;
    Field *field = nullptr;
    pos += Field::DeserializeFrom(pos, schema->GetColumn(i)->GetType(), &field, is_null);
    fields_.push_back(field);
  }
  return static_cast<uint32_t>(pos - buf);
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
  if (fields_.empty()) {
    return 0;
  }

  uint32_t field_count = static_cast<uint32_t>(fields_.size());
  uint32_t size = sizeof(uint32_t) + (field_count + 7) / 8;
  for (uint32_t i = 0; i < field_count; i++) {
    if (!fields_[i]->IsNull()) {
      size += fields_[i]->GetSerializedSize();
    }
  }
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
