#include "buffer/buffer_pool_manager.h"

#include <string>

#include "gtest/gtest.h"

TEST(BufferPoolManagerCustomTest, DeletePinnedPageAndReuseLogicalPage) {
  const std::string db_name = "bpm_custom_test.db";
  remove(db_name.c_str());

  auto *disk_manager = new DiskManager(db_name);
  auto *bpm = new BufferPoolManager(2, disk_manager);

  page_id_t page_id = INVALID_PAGE_ID;
  auto *page = bpm->NewPage(page_id);
  ASSERT_NE(nullptr, page);
  EXPECT_EQ(0, page_id);

  EXPECT_FALSE(bpm->DeletePage(page_id));
  EXPECT_TRUE(bpm->UnpinPage(page_id, true));
  EXPECT_TRUE(bpm->DeletePage(page_id));
  EXPECT_TRUE(bpm->IsPageFree(page_id));

  page_id_t reused_page_id = INVALID_PAGE_ID;
  auto *reused_page = bpm->NewPage(reused_page_id);
  ASSERT_NE(nullptr, reused_page);
  EXPECT_EQ(page_id, reused_page_id);
  EXPECT_TRUE(bpm->UnpinPage(reused_page_id, false));

  disk_manager->Close();
  remove(db_name.c_str());
  delete bpm;
  delete disk_manager;
}
