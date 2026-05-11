#include "buffer/clock_replacer.h"

CLOCKReplacer::CLOCKReplacer(size_t num_pages) : capacity(num_pages) {}

CLOCKReplacer::~CLOCKReplacer() = default;

bool CLOCKReplacer::Victim(frame_id_t *frame_id) {
  if (clock_list.empty()) {
    return false;
  }

  while (!clock_list.empty()) {
    auto candidate = clock_list.front();
    clock_list.pop_front();
    if (clock_status[candidate] == 0) {
      clock_status.erase(candidate);
      *frame_id = candidate;
      return true;
    }
    clock_status[candidate] = 0;
    clock_list.push_back(candidate);
  }

  return false;
}

void CLOCKReplacer::Pin(frame_id_t frame_id) {
  auto iter = clock_status.find(frame_id);
  if (iter == clock_status.end()) {
    return;
  }
  clock_list.remove(frame_id);
  clock_status.erase(iter);
}

void CLOCKReplacer::Unpin(frame_id_t frame_id) {
  auto iter = clock_status.find(frame_id);
  if (iter != clock_status.end()) {
    iter->second = 1;
    return;
  }
  if (clock_list.size() >= capacity) {
    return;
  }
  clock_list.push_back(frame_id);
  clock_status[frame_id] = 1;
}

size_t CLOCKReplacer::Size() { return clock_list.size(); }
