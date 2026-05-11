#include "concurrency/lock_manager.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>

#include "common/rowid.h"
#include "concurrency/txn.h"
#include "concurrency/txn_manager.h"

namespace {

constexpr auto kLockWaitInterval = std::chrono::milliseconds(50);

bool CanTakeShared(Txn *txn) {
  auto state = txn->GetState();
  auto isolation = txn->GetIsolationLevel();
  if (state == TxnState::kAborted || state == TxnState::kCommitted) {
    return false;
  }
  if (isolation == IsolationLevel::kReadUncommitted) {
    return false;
  }
  if (state == TxnState::kGrowing) {
    return true;
  }
  return isolation == IsolationLevel::kReadCommitted;
}

bool CanTakeExclusive(Txn *txn) {
  auto state = txn->GetState();
  if (state == TxnState::kAborted || state == TxnState::kCommitted) {
    return false;
  }
  return state == TxnState::kGrowing;
}

}  // namespace

void LockManager::SetTxnMgr(TxnManager *txn_mgr) { txn_mgr_ = txn_mgr; }

/**
 * TODO: Student Implement
 */
bool LockManager::LockShared(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  if (!CanTakeShared(txn)) {
    txn->SetState(TxnState::kAborted);
    auto reason = txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted
                      ? AbortReason::kLockSharedOnReadUncommitted
                      : AbortReason::kLockOnShrinking;
    throw TxnAbortException(txn->GetTxnId(), reason);
  }
  LockPrepare(txn, rid);
  if (txn->GetSharedLockSet().count(rid) != 0 || txn->GetExclusiveLockSet().count(rid) != 0) {
    return true;
  }

  auto &req_queue = lock_table_[rid];
  req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kShared);
  auto iter = req_queue.GetLockRequestIter(txn->GetTxnId());
  while (req_queue.is_writing_ || req_queue.is_upgrading_) {
    CheckAbort(txn, req_queue);
    req_queue.cv_.wait_for(lock, kLockWaitInterval);
  }
  CheckAbort(txn, req_queue);
  iter->granted_ = LockMode::kShared;
  req_queue.sharing_cnt_++;
  txn->GetSharedLockSet().emplace(rid);
  return true;
}

/**
 * TODO: Student Implement
 */
bool LockManager::LockExclusive(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  if (!CanTakeExclusive(txn)) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
  }
  LockPrepare(txn, rid);
  if (txn->GetExclusiveLockSet().count(rid) != 0) {
    return true;
  }

  auto &req_queue = lock_table_[rid];
  req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kExclusive);
  auto iter = req_queue.GetLockRequestIter(txn->GetTxnId());
  while (req_queue.is_writing_ || req_queue.sharing_cnt_ > 0) {
    CheckAbort(txn, req_queue);
    req_queue.cv_.wait_for(lock, kLockWaitInterval);
  }
  CheckAbort(txn, req_queue);
  iter->granted_ = LockMode::kExclusive;
  req_queue.is_writing_ = true;
  txn->GetExclusiveLockSet().emplace(rid);
  return true;
}

/**
 * TODO: Student Implement
 */
bool LockManager::LockUpgrade(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  if (!CanTakeExclusive(txn)) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
  }
  LockPrepare(txn, rid);
  if (txn->GetExclusiveLockSet().count(rid) != 0) {
    return true;
  }
  if (txn->GetSharedLockSet().count(rid) == 0) {
    return false;
  }

  auto &req_queue = lock_table_[rid];
  if (req_queue.is_upgrading_) {
    txn->SetState(TxnState::kAborted);
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kUpgradeConflict);
  }

  auto iter = req_queue.GetLockRequestIter(txn->GetTxnId());
  req_queue.is_upgrading_ = true;
  req_queue.sharing_cnt_--;
  txn->GetSharedLockSet().erase(rid);
  iter->lock_mode_ = LockMode::kExclusive;
  iter->granted_ = LockMode::kNone;
  iter->upgrading_ = true;

  while (req_queue.is_writing_ || req_queue.sharing_cnt_ > 0) {
    CheckAbort(txn, req_queue);
    req_queue.cv_.wait_for(lock, kLockWaitInterval);
  }
  CheckAbort(txn, req_queue);
  req_queue.is_upgrading_ = false;
  iter->upgrading_ = false;
  iter->granted_ = LockMode::kExclusive;
  req_queue.is_writing_ = true;
  txn->GetExclusiveLockSet().emplace(rid);
  return true;
}

/**
 * TODO: Student Implement
 */
bool LockManager::Unlock(Txn *txn, const RowId &rid) {
  std::unique_lock<std::mutex> lock(latch_);
  auto table_iter = lock_table_.find(rid);
  if (table_iter == lock_table_.end()) {
    return false;
  }
  auto &req_queue = table_iter->second;
  auto req_iter_map = req_queue.req_list_iter_map_.find(txn->GetTxnId());
  if (req_iter_map == req_queue.req_list_iter_map_.end()) {
    return false;
  }

  auto req_iter = req_iter_map->second;
  if (req_iter->granted_ == LockMode::kShared) {
    req_queue.sharing_cnt_--;
    txn->GetSharedLockSet().erase(rid);
  } else if (req_iter->granted_ == LockMode::kExclusive) {
    req_queue.is_writing_ = false;
    txn->GetExclusiveLockSet().erase(rid);
  } else {
    return false;
  }

  if (txn->GetState() == TxnState::kGrowing) {
    txn->SetState(TxnState::kShrinking);
  }

  req_queue.EraseLockRequest(txn->GetTxnId());
  req_queue.cv_.notify_all();
  if (req_queue.req_list_.empty()) {
    lock_table_.erase(table_iter);
  }
  return true;
}

/**
 * TODO: Student Implement
 */
void LockManager::LockPrepare(Txn *txn, const RowId &rid) {
  if (txn->GetState() == TxnState::kAborted) {
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
  }
  if (lock_table_.find(rid) == lock_table_.end()) {
    lock_table_[rid];
  }
}

/**
 * TODO: Student Implement
 */
void LockManager::CheckAbort(Txn *txn, LockManager::LockRequestQueue &req_queue) {
  if (txn->GetState() != TxnState::kAborted) {
    return;
  }
  auto iter = req_queue.req_list_iter_map_.find(txn->GetTxnId());
  if (iter != req_queue.req_list_iter_map_.end()) {
    if (iter->second->upgrading_) {
      req_queue.is_upgrading_ = false;
    }
    req_queue.EraseLockRequest(txn->GetTxnId());
    req_queue.cv_.notify_all();
  }
  throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
}

/**
 * TODO: Student Implement
 */
void LockManager::AddEdge(txn_id_t t1, txn_id_t t2) {
  if (t1 == t2) {
    return;
  }
  waits_for_[t1].emplace(t2);
}

/**
 * TODO: Student Implement
 */
void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2) {
  auto iter = waits_for_.find(t1);
  if (iter == waits_for_.end()) {
    return;
  }
  iter->second.erase(t2);
  if (iter->second.empty()) {
    waits_for_.erase(iter);
  }
}

/**
 * TODO: Student Implement
 */
bool LockManager::HasCycle(txn_id_t &newest_tid_in_cycle) {
  std::set<txn_id_t> nodes;
  for (const auto &entry : waits_for_) {
    nodes.emplace(entry.first);
    for (auto neighbor : entry.second) {
      nodes.emplace(neighbor);
    }
  }

  std::unordered_map<txn_id_t, int> color;
  std::vector<txn_id_t> path;
  std::function<bool(txn_id_t)> dfs = [&](txn_id_t txn_id) {
    color[txn_id] = 1;
    path.push_back(txn_id);
    auto iter = waits_for_.find(txn_id);
    if (iter != waits_for_.end()) {
      for (auto neighbor : iter->second) {
        if (color[neighbor] == 0) {
          if (dfs(neighbor)) {
            return true;
          }
        } else if (color[neighbor] == 1) {
          newest_tid_in_cycle = neighbor;
          for (auto rit = path.rbegin(); rit != path.rend(); ++rit) {
            newest_tid_in_cycle = std::max(newest_tid_in_cycle, *rit);
            if (*rit == neighbor) {
              break;
            }
          }
          return true;
        }
      }
    }
    path.pop_back();
    color[txn_id] = 2;
    return false;
  };

  for (auto txn_id : nodes) {
    if (color[txn_id] == 0 && dfs(txn_id)) {
      return true;
    }
  }
  newest_tid_in_cycle = INVALID_TXN_ID;
  return false;
}

void LockManager::DeleteNode(txn_id_t txn_id) {
    waits_for_.erase(txn_id);

    auto *txn = txn_mgr_->GetTransaction(txn_id);

    for (const auto &row_id: txn->GetSharedLockSet()) {
        for (const auto &lock_req: lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }

    for (const auto &row_id: txn->GetExclusiveLockSet()) {
        for (const auto &lock_req: lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }
}

/**
 * TODO: Student Implement
 */
void LockManager::RunCycleDetection() {
  while (enable_cycle_detection_) {
    std::this_thread::sleep_for(cycle_detection_interval_);
    std::unique_lock<std::mutex> lock(latch_);
    waits_for_.clear();

    for (auto &table_entry : lock_table_) {
      auto &req_queue = table_entry.second;
      for (const auto &waiter : req_queue.req_list_) {
        auto *waiter_txn = txn_mgr_->GetTransaction(waiter.txn_id_);
        if (waiter.granted_ != LockMode::kNone || waiter_txn == nullptr ||
            waiter_txn->GetState() == TxnState::kAborted) {
          continue;
        }
        for (const auto &holder : req_queue.req_list_) {
          auto *holder_txn = txn_mgr_->GetTransaction(holder.txn_id_);
          if (holder.txn_id_ == waiter.txn_id_ || holder.granted_ == LockMode::kNone || holder_txn == nullptr ||
              holder_txn->GetState() == TxnState::kAborted) {
            continue;
          }
          bool conflict = waiter.lock_mode_ == LockMode::kExclusive || holder.granted_ == LockMode::kExclusive;
          if (conflict) {
            AddEdge(waiter.txn_id_, holder.txn_id_);
          }
        }
      }
    }

    std::vector<txn_id_t> victims;
    txn_id_t newest_tid_in_cycle = INVALID_TXN_ID;
    while (HasCycle(newest_tid_in_cycle)) {
      auto *victim = txn_mgr_->GetTransaction(newest_tid_in_cycle);
      if (victim != nullptr) {
        victim->SetState(TxnState::kAborted);
        victims.push_back(newest_tid_in_cycle);
        DeleteNode(newest_tid_in_cycle);
      } else {
        break;
      }
      newest_tid_in_cycle = INVALID_TXN_ID;
    }

    if (!victims.empty()) {
      for (auto &entry : lock_table_) {
        entry.second.cv_.notify_all();
      }
    }
  }
}

/**
 * TODO: Student Implement
 */
std::vector<std::pair<txn_id_t, txn_id_t>> LockManager::GetEdgeList() {
  std::vector<std::pair<txn_id_t, txn_id_t>> result;
  for (const auto &entry : waits_for_) {
    for (auto to : entry.second) {
      result.emplace_back(entry.first, to);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}
