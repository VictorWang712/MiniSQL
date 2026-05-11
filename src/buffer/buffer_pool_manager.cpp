#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  // 1.     Search the page table for the requested page (P).
  // 1.1    If P exists, pin it and return it immediately.
  // 1.2    If P does not exist, find a replacement page (R) from either the free list or the replacer.
  //        Note that pages are always found from the free list first.
  // 2.     If R is dirty, write it back to the disk.
  // 3.     Delete R from the page table and insert P.
  // 4.     Update P's metadata, read in the page content from disk, and then return a pointer to P.
  lock_guard<recursive_mutex> lock(latch_);

  auto page_iter = page_table_.find(page_id);
  if (page_iter != page_table_.end()) {
    auto frame_id = page_iter->second;
    auto *page = &pages_[frame_id];
    page->pin_count_++;
    replacer_->Pin(frame_id);
    return page;
  }

  frame_id_t frame_id = TryToFindFreePage();
  if (frame_id == INVALID_FRAME_ID) {
    return nullptr;
  }

  auto *page = &pages_[frame_id];
  if (page->page_id_ != INVALID_PAGE_ID) {
    if (page->is_dirty_) {
      disk_manager_->WritePage(page->page_id_, page->data_);
    }
    page_table_.erase(page->page_id_);
  }

  disk_manager_->ReadPage(page_id, page->data_);
  page->page_id_ = page_id;
  page->pin_count_ = 1;
  page->is_dirty_ = false;
  page_table_[page_id] = frame_id;
  replacer_->Pin(frame_id);
  return page;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  // 0.   Make sure you call AllocatePage!
  // 1.   If all the pages in the buffer pool are pinned, return nullptr.
  // 2.   Pick a victim page P from either the free list or the replacer. Always pick from the free list first.
  // 3.   Update P's metadata, zero out memory and add P to the page table.
  // 4.   Set the page ID output parameter. Return a pointer to P.
  lock_guard<recursive_mutex> lock(latch_);

  frame_id_t frame_id = TryToFindFreePage();
  if (frame_id == INVALID_FRAME_ID) {
    return nullptr;
  }

  page_id = AllocatePage();
  auto *page = &pages_[frame_id];
  if (page->page_id_ != INVALID_PAGE_ID) {
    if (page->is_dirty_) {
      disk_manager_->WritePage(page->page_id_, page->data_);
    }
    page_table_.erase(page->page_id_);
  }

  page->ResetMemory();
  page->page_id_ = page_id;
  page->pin_count_ = 1;
  page->is_dirty_ = false;
  page_table_[page_id] = frame_id;
  replacer_->Pin(frame_id);
  return page;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  // 0.   Make sure you call DeallocatePage!
  // 1.   Search the page table for the requested page (P).
  // 1.   If P does not exist, return true.
  // 2.   If P exists, but has a non-zero pin-count, return false. Someone is using the page.
  // 3.   Otherwise, P can be deleted. Remove P from the page table, reset its metadata and return it to the free list.
  lock_guard<recursive_mutex> lock(latch_);

  auto page_iter = page_table_.find(page_id);
  if (page_iter == page_table_.end()) {
    DeallocatePage(page_id);
    return true;
  }

  auto frame_id = page_iter->second;
  auto *page = &pages_[frame_id];
  if (page->pin_count_ > 0) {
    return false;
  }

  replacer_->Pin(frame_id);
  page_table_.erase(page_iter);
  page->ResetMemory();
  page->page_id_ = INVALID_PAGE_ID;
  page->pin_count_ = 0;
  page->is_dirty_ = false;
  free_list_.push_back(frame_id);
  DeallocatePage(page_id);
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  lock_guard<recursive_mutex> lock(latch_);

  auto page_iter = page_table_.find(page_id);
  if (page_iter == page_table_.end()) {
    return false;
  }

  auto *page = &pages_[page_iter->second];
  if (page->pin_count_ <= 0) {
    return false;
  }

  page->pin_count_--;
  page->is_dirty_ = page->is_dirty_ || is_dirty;
  if (page->pin_count_ == 0) {
    replacer_->Unpin(page_iter->second);
  }
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  lock_guard<recursive_mutex> lock(latch_);

  auto page_iter = page_table_.find(page_id);
  if (page_iter == page_table_.end()) {
    return false;
  }

  auto *page = &pages_[page_iter->second];
  disk_manager_->WritePage(page_id, page->data_);
  page->is_dirty_ = false;
  return true;
}

frame_id_t BufferPoolManager::TryToFindFreePage() {
  if (!free_list_.empty()) {
    auto frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }

  frame_id_t frame_id = INVALID_FRAME_ID;
  if (replacer_->Victim(&frame_id)) {
    return frame_id;
  }
  return INVALID_FRAME_ID;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}
