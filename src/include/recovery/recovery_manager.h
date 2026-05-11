#ifndef MINISQL_RECOVERY_MANAGER_H
#define MINISQL_RECOVERY_MANAGER_H

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "recovery/log_rec.h"

using KvDatabase = std::unordered_map<KeyType, ValType>;
using ATT = std::unordered_map<txn_id_t, lsn_t>;

struct CheckPoint {
    lsn_t checkpoint_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase persist_data_{};

    inline void AddActiveTxn(txn_id_t txn_id, lsn_t last_lsn) { active_txns_[txn_id] = last_lsn; }

    inline void AddData(KeyType key, ValType val) { persist_data_.emplace(std::move(key), val); }
};

class RecoveryManager {
public:
    /**
    * TODO: Student Implement
    */
    void Init(CheckPoint &last_checkpoint) {
        persist_lsn_ = last_checkpoint.checkpoint_lsn_;
        active_txns_ = last_checkpoint.active_txns_;
        data_ = last_checkpoint.persist_data_;
    }

    /**
    * TODO: Student Implement
    */
    void RedoPhase() {
        for (const auto &entry : log_recs_) {
            if (persist_lsn_ != INVALID_LSN && entry.first < persist_lsn_) {
                continue;
            }
            ApplyRedo(entry.second);
        }
    }

    /**
    * TODO: Student Implement
    */
    void UndoPhase() {
        std::vector<std::pair<txn_id_t, lsn_t>> losers(active_txns_.begin(), active_txns_.end());
        std::sort(losers.begin(), losers.end(),
                  [](const std::pair<txn_id_t, lsn_t> &lhs, const std::pair<txn_id_t, lsn_t> &rhs) {
                      return lhs.second > rhs.second;
                  });
        for (const auto &txn : losers) {
            RollbackTxn(txn.second);
        }
        active_txns_.clear();
    }

    // used for test only
    void AppendLogRec(LogRecPtr log_rec) { log_recs_.emplace(log_rec->lsn_, log_rec); }

    // used for test only
    inline KvDatabase &GetDatabase() { return data_; }

private:
    void ApplyRedo(const LogRecPtr &log_rec) {
        switch (log_rec->type_) {
            case LogRecType::kInsert:
                data_[log_rec->new_key_] = log_rec->new_val_;
                active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                break;
            case LogRecType::kDelete:
                data_.erase(log_rec->old_key_);
                active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                break;
            case LogRecType::kUpdate:
                if (log_rec->old_key_ != log_rec->new_key_) {
                    data_.erase(log_rec->old_key_);
                }
                data_[log_rec->new_key_] = log_rec->new_val_;
                active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                break;
            case LogRecType::kBegin:
                active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                break;
            case LogRecType::kCommit:
                active_txns_.erase(log_rec->txn_id_);
                break;
            case LogRecType::kAbort:
                RollbackTxn(log_rec->prev_lsn_);
                active_txns_.erase(log_rec->txn_id_);
                break;
            case LogRecType::kInvalid:
                break;
        }
    }

    void ApplyUndo(const LogRecPtr &log_rec) {
        switch (log_rec->type_) {
            case LogRecType::kInsert:
                data_.erase(log_rec->new_key_);
                break;
            case LogRecType::kDelete:
                data_[log_rec->old_key_] = log_rec->old_val_;
                break;
            case LogRecType::kUpdate:
                if (log_rec->old_key_ != log_rec->new_key_) {
                    data_.erase(log_rec->new_key_);
                }
                data_[log_rec->old_key_] = log_rec->old_val_;
                break;
            case LogRecType::kBegin:
            case LogRecType::kCommit:
            case LogRecType::kAbort:
            case LogRecType::kInvalid:
                break;
        }
    }

    void RollbackTxn(lsn_t last_lsn) {
        lsn_t current = last_lsn;
        while (current != INVALID_LSN) {
            auto iter = log_recs_.find(current);
            if (iter == log_recs_.end()) {
                break;
            }
            const auto &log_rec = iter->second;
            ApplyUndo(log_rec);
            if (log_rec->type_ == LogRecType::kBegin) {
                break;
            }
            current = log_rec->prev_lsn_;
        }
    }

    std::map<lsn_t, LogRecPtr> log_recs_{};
    lsn_t persist_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase data_{};  // all data in database
};

#endif  // MINISQL_RECOVERY_MANAGER_H
