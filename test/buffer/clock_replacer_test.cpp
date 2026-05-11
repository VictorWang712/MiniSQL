#include "buffer/clock_replacer.h"

#include "gtest/gtest.h"

TEST(CLOCKReplacerTest, SecondChanceTest) {
  CLOCKReplacer clock_replacer(3);

  clock_replacer.Unpin(1);
  clock_replacer.Unpin(2);
  clock_replacer.Unpin(3);
  EXPECT_EQ(3, clock_replacer.Size());

  clock_replacer.Unpin(2);
  clock_replacer.Pin(3);
  clock_replacer.Unpin(3);

  frame_id_t victim = INVALID_FRAME_ID;
  ASSERT_TRUE(clock_replacer.Victim(&victim));
  EXPECT_EQ(1, victim);

  ASSERT_TRUE(clock_replacer.Victim(&victim));
  EXPECT_EQ(2, victim);

  ASSERT_TRUE(clock_replacer.Victim(&victim));
  EXPECT_EQ(3, victim);

  EXPECT_FALSE(clock_replacer.Victim(&victim));
}
