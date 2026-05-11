//
// Created by njz on 2023/1/30.
//

#include "executor/executors/update_executor.h"

namespace {
bool IsConstraintIndex(IndexInfo *index_info) {
  const std::string &name = index_info->GetIndexName();
  return name.rfind("__pk__", 0) == 0 || name.rfind("__uniq__", 0) == 0;
}
}  // namespace

UpdateExecutor::UpdateExecutor(ExecuteContext *exec_ctx, const UpdatePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

void UpdateExecutor::Init() {
  child_executor_->Init();
  exec_ctx_->GetCatalog()->GetTable(plan_->GetTableName(), table_info_);
  exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->GetTableName(), index_info_);
  txn_ = exec_ctx_->GetTransaction();
}

bool UpdateExecutor::Next([[maybe_unused]] Row *row, RowId *rid) {
  Row src_row;
  RowId src_rid;
  if (child_executor_->Next(&src_row, &src_rid)) {
    Row dest_row = GenerateUpdatedTuple(src_row);
    for (auto info : index_info_) {
      if (!IsConstraintIndex(info)) {
        continue;
      }
      Row dest_key_row;
      dest_row.GetKeyFromRow(table_info_->GetSchema(), info->GetIndexKeySchema(), dest_key_row);
      std::vector<RowId> result;
      if (info->GetIndex()->ScanKey(dest_key_row, result, txn_) == DB_SUCCESS) {
        bool has_conflict = false;
        for (const auto &existing_rid : result) {
          if (existing_rid.Get() != src_rid.Get()) {
            has_conflict = true;
            break;
          }
        }
        if (has_conflict) {
          std::cout << "key already exists" << std::endl;
          return false;
        }
      }
    }
    if (!table_info_->GetTableHeap()->UpdateTuple(dest_row, src_rid, txn_)) {
      return false;
    }
    Row src_key_row;
    Row dest_key_row;
    for (auto info : index_info_) {  // 更新索引
      src_row.GetKeyFromRow(table_info_->GetSchema(), info->GetIndexKeySchema(), src_key_row);
      dest_row.GetKeyFromRow(table_info_->GetSchema(), info->GetIndexKeySchema(), dest_key_row);
      info->GetIndex()->RemoveEntry(src_key_row, src_rid, txn_);
      info->GetIndex()->InsertEntry(dest_key_row, src_rid, txn_);
    }
    if (row != nullptr) {
      *row = Row();
    }
    return true;
  }
  return false;
}

Row UpdateExecutor::GenerateUpdatedTuple(const Row &src_row) {
  const auto update_attrs = plan_->GetUpdateAttr();
  Schema *schema = table_info_->GetSchema();
  uint32_t col_count = schema->GetColumnCount();
  std::vector<Field> values;
  for (uint32_t idx = 0; idx < col_count; idx++) {
    if (update_attrs.find(idx) == update_attrs.cend()) {
      values.emplace_back(*src_row.GetField(idx));
    } else {
      auto expr = update_attrs.at(idx);
      values.emplace_back(expr->Evaluate(&src_row));
    }
  }
  return Row{values};
}
