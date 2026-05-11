#include "concurrency/lock_manager.h"

#include "concurrency/txn.h"
#include "concurrency/txn_manager.h"
#include "gtest/gtest.h"

class LockManagerExtraTest : public testing::Test {
 protected:
  void SetUp() override {
    lock_mgr_ = new LockManager();
    txn_mgr_ = new TxnManager(lock_mgr_);
  }

  void TearDown() override {
    delete txn_mgr_;
    delete lock_mgr_;
  }

  LockManager *lock_mgr_{nullptr};
  TxnManager *txn_mgr_{nullptr};
};

TEST_F(LockManagerExtraTest, ReadCommittedCanRelockSharedAfterShrinking) {
  RowId r0(0, 0);
  RowId r1(0, 1);
  Txn *txn = txn_mgr_->Begin(nullptr, IsolationLevel::kReadCommitted);

  ASSERT_TRUE(lock_mgr_->LockShared(txn, r0));
  ASSERT_TRUE(lock_mgr_->Unlock(txn, r0));
  ASSERT_EQ(TxnState::kShrinking, txn->GetState());

  ASSERT_TRUE(lock_mgr_->LockShared(txn, r1));
  ASSERT_EQ(TxnState::kShrinking, txn->GetState());
  ASSERT_EQ(txn->GetSharedLockSet().count(r1), 1);

  txn_mgr_->Commit(txn);
  ASSERT_EQ(TxnState::kCommitted, txn->GetState());
  ASSERT_TRUE(txn->GetSharedLockSet().empty());

  delete txn;
}
