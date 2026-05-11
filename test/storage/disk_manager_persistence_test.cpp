#include "storage/disk_manager.h"

#include <string>

#include "gtest/gtest.h"

TEST(DiskManagerPersistenceTest, ReopenAndReuseFreedPage) {
  std::string db_name = "disk_persistence_test.db";
  remove(db_name.c_str());

  {
    DiskManager disk_mgr(db_name);
    page_id_t page0 = disk_mgr.AllocatePage();
    page_id_t page1 = disk_mgr.AllocatePage();
    EXPECT_EQ(0, page0);
    EXPECT_EQ(1, page1);

    char write_buf[PAGE_SIZE];
    memset(write_buf, 0, sizeof(write_buf));
    memcpy(write_buf, "persisted-page", strlen("persisted-page"));
    disk_mgr.WritePage(page1, write_buf);
    disk_mgr.Close();
  }

  {
    DiskManager disk_mgr(db_name);
    EXPECT_FALSE(disk_mgr.IsPageFree(0));
    EXPECT_FALSE(disk_mgr.IsPageFree(1));

    char read_buf[PAGE_SIZE];
    memset(read_buf, 0, sizeof(read_buf));
    disk_mgr.ReadPage(1, read_buf);
    EXPECT_STREQ("persisted-page", read_buf);

    disk_mgr.DeAllocatePage(0);
    EXPECT_TRUE(disk_mgr.IsPageFree(0));
    EXPECT_EQ(0, disk_mgr.AllocatePage());
    disk_mgr.Close();
  }

  remove(db_name.c_str());
}
