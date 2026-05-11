#include "page/bitmap_page.h"

#include "glog/logging.h"

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  if (page_allocated_ >= GetMaxSupportedSize()) {
    return false;
  }
  for (uint32_t offset = next_free_page_; offset < GetMaxSupportedSize(); offset++) {
    if (IsPageFree(offset)) {
      auto byte_index = offset / 8;
      auto bit_index = static_cast<uint8_t>(offset % 8);
      bytes[byte_index] |= static_cast<unsigned char>(1U << bit_index);
      page_allocated_++;
      page_offset = offset;

      next_free_page_ = GetMaxSupportedSize();
      for (uint32_t next = offset + 1; next < GetMaxSupportedSize(); next++) {
        if (IsPageFree(next)) {
          next_free_page_ = next;
          break;
        }
      }
      return true;
    }
  }
  return false;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  if (page_offset >= GetMaxSupportedSize() || IsPageFree(page_offset)) {
    return false;
  }
  auto byte_index = page_offset / 8;
  auto bit_index = static_cast<uint8_t>(page_offset % 8);
  bytes[byte_index] &= static_cast<unsigned char>(~(1U << bit_index));
  page_allocated_--;
  if (page_offset < next_free_page_) {
    next_free_page_ = page_offset;
  }
  return true;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  if (page_offset >= GetMaxSupportedSize()) {
    return false;
  }
  return IsPageFreeLow(page_offset / 8, static_cast<uint8_t>(page_offset % 8));
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  return (bytes[byte_index] & static_cast<unsigned char>(1U << bit_index)) == 0;
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;
