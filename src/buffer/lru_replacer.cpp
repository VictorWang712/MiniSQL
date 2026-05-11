#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) : capacity_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

/**
 * TODO: Student Implement
 */
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  lock_guard<mutex> lock(latch_);
  if (lru_list_.empty()) {
    return false;
  }
  *frame_id = lru_list_.front();
  lru_map_.erase(*frame_id);
  lru_list_.pop_front();
  return true;
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Pin(frame_id_t frame_id) {
  lock_guard<mutex> lock(latch_);
  auto iter = lru_map_.find(frame_id);
  if (iter == lru_map_.end()) {
    return;
  }
  lru_list_.erase(iter->second);
  lru_map_.erase(iter);
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
  lock_guard<mutex> lock(latch_);
  if (lru_map_.find(frame_id) != lru_map_.end()) {
    return;
  }
  if (lru_list_.size() >= capacity_) {
    return;
  }
  lru_list_.push_back(frame_id);
  lru_map_[frame_id] = prev(lru_list_.end());
}

/**
 * TODO: Student Implement
 */
size_t LRUReplacer::Size() {
  lock_guard<mutex> lock(latch_);
  return lru_list_.size();
}
