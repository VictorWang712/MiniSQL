#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};

// used for testing only
using KeyType = std::string;
using ValType = int32_t;

/**
 * TODO: Student Implement
 */
struct LogRec {
    LogRec() = default;

    LogRecType type_{LogRecType::kInvalid};
    txn_id_t txn_id_{INVALID_TXN_ID};
    lsn_t lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};
    KeyType old_key_{};
    ValType old_val_{};
    KeyType new_key_{};
    ValType new_val_{};

    /* used for testing only */
    static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
    static lsn_t next_lsn_;
};

typedef std::shared_ptr<LogRec> LogRecPtr;

inline LogRecPtr CreateLog(LogRecType type, txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = type;
    log->txn_id_ = txn_id;
    auto iter = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = (iter == LogRec::prev_lsn_map_.end()) ? INVALID_LSN : iter->second;
    log->lsn_ = LogRec::next_lsn_++;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

/**
 * TODO: Student Implement
 */
inline LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
    auto log = CreateLog(LogRecType::kInsert, txn_id);
    log->new_key_ = std::move(ins_key);
    log->new_val_ = ins_val;
    return log;
}

/**
 * TODO: Student Implement
 */
inline LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
    auto log = CreateLog(LogRecType::kDelete, txn_id);
    log->old_key_ = std::move(del_key);
    log->old_val_ = del_val;
    return log;
}

/**
 * TODO: Student Implement
 */
inline LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val) {
    auto log = CreateLog(LogRecType::kUpdate, txn_id);
    log->old_key_ = std::move(old_key);
    log->old_val_ = old_val;
    log->new_key_ = std::move(new_key);
    log->new_val_ = new_val;
    return log;
}

/**
 * TODO: Student Implement
 */
inline LogRecPtr CreateBeginLog(txn_id_t txn_id) {
    return CreateLog(LogRecType::kBegin, txn_id);
}

/**
 * TODO: Student Implement
 */
inline LogRecPtr CreateCommitLog(txn_id_t txn_id) {
    return CreateLog(LogRecType::kCommit, txn_id);
}

/**
 * TODO: Student Implement
 */
inline LogRecPtr CreateAbortLog(txn_id_t txn_id) {
    return CreateLog(LogRecType::kAbort, txn_id);
}

#endif  // MINISQL_LOG_REC_H
