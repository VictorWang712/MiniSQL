#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

/**
 * TODO: Student Implement
 */
uint32_t CatalogMeta::GetSerializedSize() const {
  return sizeof(uint32_t) * 3 +
         static_cast<uint32_t>((table_meta_pages_.size() + index_meta_pages_.size()) * (sizeof(uint32_t) + sizeof(page_id_t)));
}

CatalogMeta::CatalogMeta() {}

/**
 * TODO: Student Implement
 */
CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  if (init) {
    catalog_meta_ = CatalogMeta::NewInstance();
    next_table_id_ = 0;
    next_index_id_ = 0;
    FlushCatalogMetaPage();
    return;
  }

  auto *page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  ASSERT(page != nullptr, "Cannot fetch catalog meta page.");
  catalog_meta_ = CatalogMeta::DeserializeFrom(page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);

  next_table_id_ = catalog_meta_->GetNextTableId();
  next_index_id_ = catalog_meta_->GetNextIndexId();

  for (const auto &entry : *catalog_meta_->GetTableMetaPages()) {
    LoadTable(entry.first, entry.second);
  }
  for (const auto &entry : *catalog_meta_->GetIndexMetaPages()) {
    LoadIndex(entry.first, entry.second);
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
  if (table_names_.find(table_name) != table_names_.end()) {
    return DB_TABLE_ALREADY_EXIST;
  }

  auto schema_copy = Schema::DeepCopySchema(schema);
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, schema_copy, txn, log_manager_, lock_manager_);
  auto table_id = next_table_id_++;
  auto *table_meta = TableMetadata::Create(table_id, table_name, table_heap->GetFirstPageId(), schema_copy);
  auto *info = TableInfo::Create();
  info->Init(table_meta, table_heap);

  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    delete info;
    return DB_FAILED;
  }
  table_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  catalog_meta_->table_meta_pages_[table_id] = meta_page_id;
  table_names_[table_name] = table_id;
  tables_[table_id] = info;
  table_info = info;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto iter = table_names_.find(table_name);
  if (iter == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = tables_.at(iter->second);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  for (const auto &entry : tables_) {
    tables.push_back(entry.second);
  }
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
  (void)index_type;
  TableInfo *table_info = nullptr;
  auto table_result = GetTable(table_name, table_info);
  if (table_result != DB_SUCCESS) {
    return table_result;
  }

  auto &table_indexes = index_names_[table_name];
  if (table_indexes.find(index_name) != table_indexes.end()) {
    return DB_INDEX_ALREADY_EXIST;
  }

  std::vector<uint32_t> key_map;
  key_map.reserve(index_keys.size());
  for (const auto &key_name : index_keys) {
    uint32_t column_index;
    auto column_result = table_info->GetSchema()->GetColumnIndex(key_name, column_index);
    if (column_result != DB_SUCCESS) {
      return column_result;
    }
    key_map.push_back(column_index);
  }

  auto index_id = next_index_id_++;
  auto *meta = IndexMetadata::Create(index_id, index_name, table_info->GetTableId(), key_map);
  auto *info = IndexInfo::Create();
  info->Init(meta, table_info, buffer_pool_manager_);

  for (auto iter = table_info->GetTableHeap()->Begin(txn); iter != table_info->GetTableHeap()->End(); ++iter) {
    Row table_row = *iter;
    Row key_row;
    table_row.GetKeyFromRow(table_info->GetSchema(), info->GetIndexKeySchema(), key_row);
    if (info->GetIndex()->InsertEntry(key_row, table_row.GetRowId(), txn) != DB_SUCCESS) {
      delete info;
      return DB_FAILED;
    }
  }

  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    delete info;
    return DB_FAILED;
  }
  meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  catalog_meta_->index_meta_pages_[index_id] = meta_page_id;
  table_indexes[index_name] = index_id;
  indexes_[index_id] = info;
  index_info = info;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  auto table_iter = index_names_.find(table_name);
  if (table_iter == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  auto index_iter = table_iter->second.find(index_name);
  if (index_iter == table_iter->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  index_info = indexes_.at(index_iter->second);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  if (table_names_.find(table_name) == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  auto table_iter = index_names_.find(table_name);
  if (table_iter == index_names_.end()) {
    return DB_SUCCESS;
  }
  for (const auto &entry : table_iter->second) {
    indexes.push_back(this->indexes_.at(entry.second));
  }
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropTable(const string &table_name) {
  auto iter = table_names_.find(table_name);
  if (iter == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  return DropTable(iter->second);
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto table_iter = index_names_.find(table_name);
  if (table_iter == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  auto index_iter = table_iter->second.find(index_name);
  if (index_iter == table_iter->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  index_id_t index_id = index_iter->second;
  auto *info = indexes_.at(index_id);
  info->GetIndex()->Destroy();
  delete info;
  indexes_.erase(index_id);
  table_iter->second.erase(index_iter);
  if (table_iter->second.empty()) {
    index_names_.erase(table_iter);
  }
  catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, index_id);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::FlushCatalogMetaPage() const {
  auto *page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  if (page == nullptr) {
    return DB_FAILED;
  }
  catalog_meta_->SerializeTo(page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }
  TableMetadata *meta = nullptr;
  TableMetadata::DeserializeFrom(page->GetData(), meta);
  buffer_pool_manager_->UnpinPage(page_id, false);

  auto *table_heap =
      TableHeap::Create(buffer_pool_manager_, meta->GetFirstPageId(), meta->GetSchema(), log_manager_, lock_manager_);
  auto *info = TableInfo::Create();
  info->Init(meta, table_heap);

  table_names_[meta->GetTableName()] = table_id;
  tables_[table_id] = info;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }
  IndexMetadata *meta = nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(), meta);
  buffer_pool_manager_->UnpinPage(page_id, false);

  TableInfo *table_info = nullptr;
  auto table_result = GetTable(meta->GetTableId(), table_info);
  if (table_result != DB_SUCCESS) {
    delete meta;
    return table_result;
  }

  auto *info = IndexInfo::Create();
  info->Init(meta, table_info, buffer_pool_manager_);

  index_names_[table_info->GetTableName()][meta->GetIndexName()] = index_id;
  indexes_[index_id] = info;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto iter = tables_.find(table_id);
  if (iter == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = iter->second;
  return DB_SUCCESS;
}

dberr_t CatalogManager::DropTable(table_id_t table_id) {
  auto table_iter = tables_.find(table_id);
  if (table_iter == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  TableInfo *table_info = table_iter->second;
  std::string table_name = table_info->GetTableName();
  std::vector<std::string> index_names;
  auto index_name_iter = index_names_.find(table_name);
  if (index_name_iter != index_names_.end()) {
    for (const auto &entry : index_name_iter->second) {
      index_names.push_back(entry.first);
    }
  }
  for (const auto &name : index_names) {
    DropIndex(table_name, name);
  }

  auto meta_page_iter = catalog_meta_->table_meta_pages_.find(table_id);
  if (meta_page_iter != catalog_meta_->table_meta_pages_.end()) {
    buffer_pool_manager_->DeletePage(meta_page_iter->second);
    catalog_meta_->table_meta_pages_.erase(meta_page_iter);
  }
  table_info->GetTableHeap()->FreeTableHeap();
  table_names_.erase(table_name);
  tables_.erase(table_iter);
  delete table_info;
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}
