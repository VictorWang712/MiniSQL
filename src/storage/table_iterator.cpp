#include "storage/table_iterator.h"

#include "common/macros.h"
#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), row_(rid), txn_(txn) {
  if (table_heap_ != nullptr && rid.GetPageId() != INVALID_PAGE_ID) {
    table_heap_->GetTuple(&row_, txn_);
  }
}

TableIterator::TableIterator(const TableIterator &other)
    : table_heap_(other.table_heap_), row_(other.row_), txn_(other.txn_) {}

TableIterator::~TableIterator() = default;

bool TableIterator::operator==(const TableIterator &itr) const {
  return table_heap_ == itr.table_heap_ && row_.GetRowId() == itr.row_.GetRowId();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this == itr);
}

const Row &TableIterator::operator*() {
  return row_;
}

Row *TableIterator::operator->() {
  return &row_;
}

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  if (this != &itr) {
    table_heap_ = itr.table_heap_;
    row_ = itr.row_;
    txn_ = itr.txn_;
  }
  return *this;
}

// ++iter
TableIterator &TableIterator::operator++() {
  if (table_heap_ == nullptr || row_.GetRowId().GetPageId() == INVALID_PAGE_ID) {
    return *this;
  }

  page_id_t page_id = row_.GetRowId().GetPageId();
  RowId next_rid;
  while (page_id != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(table_heap_->buffer_pool_manager_->FetchPage(page_id));
    if (page == nullptr) {
      row_ = Row(INVALID_ROWID);
      return *this;
    }

    page_id_t next_page_id = page->GetNextPageId();
    bool found = false;
    if (page_id == row_.GetRowId().GetPageId()) {
      found = page->GetNextTupleRid(row_.GetRowId(), &next_rid);
    } else {
      found = page->GetFirstTupleRid(&next_rid);
    }
    table_heap_->buffer_pool_manager_->UnpinPage(page_id, false);

    if (found) {
      row_ = Row(next_rid);
      table_heap_->GetTuple(&row_, txn_);
      return *this;
    }
    page_id = next_page_id;
  }

  row_ = Row(INVALID_ROWID);
  return *this;
}

// iter++
TableIterator TableIterator::operator++(int) {
  TableIterator temp(*this);
  ++(*this);
  return TableIterator(temp);
}
