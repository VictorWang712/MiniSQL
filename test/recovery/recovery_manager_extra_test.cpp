#include "recovery/recovery_manager.h"

#include "gtest/gtest.h"
#include "recovery/log_rec.h"

class RecoveryManagerExtraTest : public testing::Test {
 protected:
  void SetUp() override {
    LogRec::prev_lsn_map_.clear();
    LogRec::next_lsn_ = 0;
  }
};

TEST_F(RecoveryManagerExtraTest, AbortRedoHandlesUpdateKeyRename) {
  auto d0 = CreateBeginLog(10);
  auto d1 = CreateInsertLog(10, "A", 1);
  auto d2 = CreateUpdateLog(10, "A", 1, "B", 2);
  auto d3 = CreateAbortLog(10);

  RecoveryManager recovery_mgr;
  CheckPoint checkpoint;
  recovery_mgr.Init(checkpoint);
  recovery_mgr.AppendLogRec(d0);
  recovery_mgr.AppendLogRec(d1);
  recovery_mgr.AppendLogRec(d2);
  recovery_mgr.AppendLogRec(d3);

  recovery_mgr.RedoPhase();
  auto &db = recovery_mgr.GetDatabase();
  ASSERT_TRUE(db.empty());

  recovery_mgr.UndoPhase();
  ASSERT_TRUE(db.empty());
}

TEST_F(RecoveryManagerExtraTest, UndoPhaseRollsBackAllLoserTransactions) {
  auto d0 = CreateBeginLog(1);
  auto d1 = CreateInsertLog(1, "A", 10);
  auto d2 = CreateBeginLog(2);
  auto d3 = CreateInsertLog(2, "B", 20);
  auto d4 = CreateUpdateLog(2, "B", 20, "B", 25);

  RecoveryManager recovery_mgr;
  CheckPoint checkpoint;
  recovery_mgr.Init(checkpoint);
  recovery_mgr.AppendLogRec(d0);
  recovery_mgr.AppendLogRec(d1);
  recovery_mgr.AppendLogRec(d2);
  recovery_mgr.AppendLogRec(d3);
  recovery_mgr.AppendLogRec(d4);

  recovery_mgr.RedoPhase();
  auto &db = recovery_mgr.GetDatabase();
  ASSERT_EQ(db["A"], 10);
  ASSERT_EQ(db["B"], 25);

  recovery_mgr.UndoPhase();
  ASSERT_EQ(db.count("A"), 0);
  ASSERT_EQ(db.count("B"), 0);
}
