#include "record/schema.h"

/**
 * TODO: Student Implement
 */
uint32_t Schema::SerializeTo(char *buf) const {
  char *pos = buf;
  MACH_WRITE_UINT32(pos, SCHEMA_MAGIC_NUM);
  pos += sizeof(uint32_t);
  MACH_WRITE_UINT32(pos, columns_.size());
  pos += sizeof(uint32_t);
  for (const auto &column : columns_) {
    pos += column->SerializeTo(pos);
  }
  return static_cast<uint32_t>(pos - buf);
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size = sizeof(uint32_t) * 2;
  for (const auto &column : columns_) {
    size += column->GetSerializedSize();
  }
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  if (schema != nullptr) {
    LOG(WARNING) << "Pointer to schema is not null in schema deserialize." << std::endl;
  }

  char *pos = buf;
  uint32_t magic_num = MACH_READ_UINT32(pos);
  pos += sizeof(uint32_t);
  ASSERT(magic_num == SCHEMA_MAGIC_NUM, "Failed to deserialize schema.");

  uint32_t column_count = MACH_READ_UINT32(pos);
  pos += sizeof(uint32_t);
  std::vector<Column *> columns;
  columns.reserve(column_count);
  for (uint32_t i = 0; i < column_count; i++) {
    Column *column = nullptr;
    pos += Column::DeserializeFrom(pos, column);
    columns.push_back(column);
  }
  schema = new Schema(columns, true);
  return static_cast<uint32_t>(pos - buf);
}
