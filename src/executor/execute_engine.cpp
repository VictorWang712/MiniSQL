#include "executor/execute_engine.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include "common/result_writer.h"
#include "executor/executors/delete_executor.h"
#include "executor/executors/index_scan_executor.h"
#include "executor/executors/insert_executor.h"
#include "executor/executors/seq_scan_executor.h"
#include "executor/executors/update_executor.h"
#include "executor/executors/values_executor.h"
#include "glog/logging.h"
#include "planner/planner.h"
#include "utils/utils.h"

extern "C" {
#include "parser/minisql_lex.h"
int yyparse(void);
}

namespace {
constexpr const char *kPrimaryIndexPrefix = "__pk__";
constexpr const char *kUniqueIndexPrefix = "__uniq__";

bool ParsePositiveInteger(const char *text, uint32_t &value) {
  if (text == nullptr) {
    return false;
  }
  char *end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (*text == '\0' || *end != '\0' || parsed <= 0) {
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

std::string NormalizeIndexType(const std::string &index_type) {
  if (index_type == "btree") {
    return "bptree";
  }
  return index_type;
}
}  // namespace

ExecuteEngine::ExecuteEngine() {
  char path[] = "./databases";
  DIR *dir;
  if ((dir = opendir(path)) == nullptr) {
    mkdir("./databases", 0777);
    dir = opendir(path);
  }
  struct dirent *stdir;
  while ((stdir = readdir(dir)) != nullptr) {
    if (strcmp(stdir->d_name, ".") == 0 || strcmp(stdir->d_name, "..") == 0 || stdir->d_name[0] == '.') {
      continue;
    }
    dbs_[stdir->d_name] = nullptr;
  }
  closedir(dir);
}

std::unique_ptr<AbstractExecutor> ExecuteEngine::CreateExecutor(ExecuteContext *exec_ctx,
                                                                const AbstractPlanNodeRef &plan) {
  switch (plan->GetType()) {
    // Create a new sequential scan executor
    case PlanType::SeqScan: {
      return std::make_unique<SeqScanExecutor>(exec_ctx, dynamic_cast<const SeqScanPlanNode *>(plan.get()));
    }
    // Create a new index scan executor
    case PlanType::IndexScan: {
      return std::make_unique<IndexScanExecutor>(exec_ctx, dynamic_cast<const IndexScanPlanNode *>(plan.get()));
    }
    // Create a new update executor
    case PlanType::Update: {
      auto update_plan = dynamic_cast<const UpdatePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, update_plan->GetChildPlan());
      return std::make_unique<UpdateExecutor>(exec_ctx, update_plan, std::move(child_executor));
    }
      // Create a new delete executor
    case PlanType::Delete: {
      auto delete_plan = dynamic_cast<const DeletePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, delete_plan->GetChildPlan());
      return std::make_unique<DeleteExecutor>(exec_ctx, delete_plan, std::move(child_executor));
    }
    case PlanType::Insert: {
      auto insert_plan = dynamic_cast<const InsertPlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, insert_plan->GetChildPlan());
      return std::make_unique<InsertExecutor>(exec_ctx, insert_plan, std::move(child_executor));
    }
    case PlanType::Values: {
      return std::make_unique<ValuesExecutor>(exec_ctx, dynamic_cast<const ValuesPlanNode *>(plan.get()));
    }
    default:
      throw std::logic_error("Unsupported plan type.");
  }
}

dberr_t ExecuteEngine::ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                                   ExecuteContext *exec_ctx) {
  // Construct the executor for the abstract plan node
  auto executor = CreateExecutor(exec_ctx, plan);

  try {
    executor->Init();
    RowId rid{};
    Row row{};
    while (executor->Next(&row, &rid)) {
      if (result_set != nullptr) {
        result_set->push_back(row);
      }
    }
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Executor Execution: " << ex.what() << std::endl;
    if (result_set != nullptr) {
      result_set->clear();
    }
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::Execute(pSyntaxNode ast) {
  if (ast == nullptr) {
    return DB_FAILED;
  }
  auto start_time = std::chrono::system_clock::now();
  unique_ptr<ExecuteContext> context(nullptr);
  if (!current_db_.empty()) {
    if (dbs_[current_db_] == nullptr) {
      dbs_[current_db_] = new DBStorageEngine(current_db_, false);
    }
    context = dbs_[current_db_]->MakeExecuteContext(nullptr);
  }
  switch (ast->type_) {
    case kNodeCreateDB:
      return ExecuteCreateDatabase(ast, context.get());
    case kNodeDropDB:
      return ExecuteDropDatabase(ast, context.get());
    case kNodeShowDB:
      return ExecuteShowDatabases(ast, context.get());
    case kNodeUseDB:
      return ExecuteUseDatabase(ast, context.get());
    case kNodeShowTables:
      return ExecuteShowTables(ast, context.get());
    case kNodeCreateTable:
      return ExecuteCreateTable(ast, context.get());
    case kNodeDropTable:
      return ExecuteDropTable(ast, context.get());
    case kNodeShowIndexes:
      return ExecuteShowIndexes(ast, context.get());
    case kNodeCreateIndex:
      return ExecuteCreateIndex(ast, context.get());
    case kNodeDropIndex:
      return ExecuteDropIndex(ast, context.get());
    case kNodeTrxBegin:
      return ExecuteTrxBegin(ast, context.get());
    case kNodeTrxCommit:
      return ExecuteTrxCommit(ast, context.get());
    case kNodeTrxRollback:
      return ExecuteTrxRollback(ast, context.get());
    case kNodeExecFile:
      return ExecuteExecfile(ast, context.get());
    case kNodeQuit:
      return ExecuteQuit(ast, context.get());
    default:
      break;
  }
  if (context == nullptr) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }
  // Plan the query.
  Planner planner(context.get());
  std::vector<Row> result_set{};
  try {
    planner.PlanQuery(ast);
    // Execute the query.
    ExecutePlan(planner.plan_, &result_set, nullptr, context.get());
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Planner: " << ex.what() << std::endl;
    return DB_FAILED;
  }
  auto stop_time = std::chrono::system_clock::now();
  double duration_time =
      double((std::chrono::duration_cast<std::chrono::microseconds>(stop_time - start_time)).count());
  // Return the result set as string.
  std::stringstream ss;
  ResultWriter writer(ss);

  if (planner.plan_->GetType() == PlanType::SeqScan || planner.plan_->GetType() == PlanType::IndexScan) {
    auto schema = planner.plan_->OutputSchema();
    auto num_of_columns = schema->GetColumnCount();
    if (!result_set.empty()) {
      // find the max width for each column
      vector<int> data_width(num_of_columns, 0);
      for (const auto &row : result_set) {
        for (uint32_t i = 0; i < num_of_columns; i++) {
          data_width[i] = max(data_width[i], int(row.GetField(i)->toString().size()));
        }
      }
      int k = 0;
      for (const auto &column : schema->GetColumns()) {
        data_width[k] = max(data_width[k], int(column->GetName().length()));
        k++;
      }
      // Generate header for the result set.
      writer.Divider(data_width);
      k = 0;
      writer.BeginRow();
      for (const auto &column : schema->GetColumns()) {
        writer.WriteHeaderCell(column->GetName(), data_width[k++]);
      }
      writer.EndRow();
      writer.Divider(data_width);

      // Transforming result set into strings.
      for (const auto &row : result_set) {
        writer.BeginRow();
        for (uint32_t i = 0; i < schema->GetColumnCount(); i++) {
          writer.WriteCell(row.GetField(i)->toString(), data_width[i]);
        }
        writer.EndRow();
      }
      writer.Divider(data_width);
    }
    writer.EndInformation(result_set.size(), duration_time, true);
  } else {
    writer.EndInformation(result_set.size(), duration_time, false);
  }
  std::cout << writer.stream_.rdbuf();
  // todo:: use shared_ptr for schema
  if (ast->type_ == kNodeSelect)
      delete planner.plan_->OutputSchema();
  return DB_SUCCESS;
}

void ExecuteEngine::ExecuteInformation(dberr_t result) {
  switch (result) {
    case DB_ALREADY_EXIST:
      cout << "Database already exists." << endl;
      break;
    case DB_NOT_EXIST:
      cout << "Database not exists." << endl;
      break;
    case DB_TABLE_ALREADY_EXIST:
      cout << "Table already exists." << endl;
      break;
    case DB_TABLE_NOT_EXIST:
      cout << "Table not exists." << endl;
      break;
    case DB_INDEX_ALREADY_EXIST:
      cout << "Index already exists." << endl;
      break;
    case DB_INDEX_NOT_FOUND:
      cout << "Index not exists." << endl;
      break;
    case DB_COLUMN_NAME_NOT_EXIST:
      cout << "Column not exists." << endl;
      break;
    case DB_KEY_NOT_FOUND:
      cout << "Key not exists." << endl;
      break;
    case DB_QUIT:
      cout << "Bye." << endl;
      break;
    default:
      break;
  }
}

dberr_t ExecuteEngine::ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    return DB_ALREADY_EXIST;
  }
  dbs_.insert(make_pair(db_name, new DBStorageEngine(db_name, true)));
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) == dbs_.end()) {
    return DB_NOT_EXIST;
  }
  remove(("./databases/" + db_name).c_str());
  if (dbs_[db_name] != nullptr) {
    delete dbs_[db_name];
  }
  dbs_.erase(db_name);
  if (db_name == current_db_)
    current_db_ = "";
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowDatabases" << std::endl;
#endif
  if (dbs_.empty()) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_SUCCESS;
  }
  int max_width = 8;
  for (const auto &itr : dbs_) {
    if (itr.first.length() > max_width) max_width = itr.first.length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << "Database"
       << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : dbs_) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteUseDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    if (dbs_[db_name] == nullptr) {
      dbs_[db_name] = new DBStorageEngine(db_name, false);
    }
    current_db_ = db_name;
    cout << "Database changed" << endl;
    return DB_SUCCESS;
  }
  return DB_NOT_EXIST;
}

dberr_t ExecuteEngine::ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowTables" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  vector<TableInfo *> tables;
  if (dbs_[current_db_]->catalog_mgr_->GetTables(tables) == DB_FAILED) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_FAILED;
  }
  string table_in_db("Tables_in_" + current_db_);
  uint max_width = table_in_db.length();
  for (const auto &itr : tables) {
    if (itr->GetTableName().length() > max_width) max_width = itr->GetTableName().length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << table_in_db << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : tables) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr->GetTableName() << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateTable" << std::endl;
#endif
  if (context == nullptr) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  std::string table_name = ast->child_->val_;
  pSyntaxNode definition = ast->child_->next_ == nullptr ? nullptr : ast->child_->next_->child_;
  std::vector<Column *> columns;
  std::vector<std::string> unique_columns;
  std::vector<std::string> primary_keys;
  uint32_t column_index = 0;

  while (definition != nullptr) {
    if (definition->type_ == kNodeColumnDefinition) {
      std::string column_name = definition->child_->val_;
      pSyntaxNode type_node = definition->child_->next_;
      bool is_unique = definition->val_ != nullptr && strcmp(definition->val_, "unique") == 0;
      if (type_node->val_ == nullptr) {
        for (auto *column : columns) {
          delete column;
        }
        return DB_FAILED;
      }

      if (strcmp(type_node->val_, "int") == 0) {
        columns.push_back(new Column(column_name, TypeId::kTypeInt, column_index, true, is_unique));
      } else if (strcmp(type_node->val_, "float") == 0) {
        columns.push_back(new Column(column_name, TypeId::kTypeFloat, column_index, true, is_unique));
      } else if (strcmp(type_node->val_, "char") == 0) {
        uint32_t char_len = 0;
        if (type_node->child_ == nullptr || !ParsePositiveInteger(type_node->child_->val_, char_len)) {
          for (auto *column : columns) {
            delete column;
          }
          return DB_FAILED;
        }
        columns.push_back(new Column(column_name, TypeId::kTypeChar, char_len, column_index, true, is_unique));
      } else {
        for (auto *column : columns) {
          delete column;
        }
        return DB_FAILED;
      }

      if (is_unique) {
        unique_columns.push_back(column_name);
      }
      column_index++;
    } else if (definition->type_ == kNodeColumnList && definition->val_ != nullptr &&
               strcmp(definition->val_, "primary keys") == 0) {
      pSyntaxNode key = definition->child_;
      while (key != nullptr) {
        primary_keys.emplace_back(key->val_);
        key = key->next_;
      }
    }
    definition = definition->next_;
  }

  std::unique_ptr<TableSchema> schema(new Schema(columns));
  TableInfo *table_info = nullptr;
  auto result = context->GetCatalog()->CreateTable(table_name, schema.get(), context->GetTransaction(), table_info);
  if (result != DB_SUCCESS) {
    return result;
  }

  for (const auto &column_name : unique_columns) {
    IndexInfo *index_info = nullptr;
    auto unique_result = context->GetCatalog()->CreateIndex(
        table_name, std::string(kUniqueIndexPrefix) + table_name + "_" + column_name, {column_name},
        context->GetTransaction(), index_info, "bptree");
    if (unique_result != DB_SUCCESS) {
      return unique_result;
    }
  }
  if (!primary_keys.empty()) {
    IndexInfo *index_info = nullptr;
    auto pk_result = context->GetCatalog()->CreateIndex(table_name, std::string(kPrimaryIndexPrefix) + table_name,
                                                        primary_keys, context->GetTransaction(), index_info, "bptree");
    if (pk_result != DB_SUCCESS) {
      return pk_result;
    }
  }
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropTable" << std::endl;
#endif
  if (context == nullptr) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }
  return context->GetCatalog()->DropTable(ast->child_->val_);
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowIndexes" << std::endl;
#endif
  if (context == nullptr) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  std::vector<TableInfo *> tables;
  context->GetCatalog()->GetTables(tables);
  std::vector<std::pair<std::string, std::string>> rows;
  for (auto *table : tables) {
    std::vector<IndexInfo *> indexes;
    context->GetCatalog()->GetTableIndexes(table->GetTableName(), indexes);
    for (auto *index : indexes) {
      rows.emplace_back(table->GetTableName(), index->GetIndexName());
    }
  }
  if (rows.empty()) {
    std::cout << "Empty set (0.00 sec)" << std::endl;
    return DB_SUCCESS;
  }

  size_t table_width = strlen("table");
  size_t index_width = strlen("index");
  for (const auto &row : rows) {
    table_width = std::max(table_width, row.first.size());
    index_width = std::max(index_width, row.second.size());
  }
  std::cout << "+" << setfill('-') << setw(table_width + 2) << "" << "+"
            << setfill('-') << setw(index_width + 2) << "" << "+" << std::endl;
  std::cout << "| " << std::left << setfill(' ') << setw(table_width) << "table"
            << " | " << setw(index_width) << "index"
            << " |" << std::endl;
  std::cout << "+" << setfill('-') << setw(table_width + 2) << "" << "+"
            << setfill('-') << setw(index_width + 2) << "" << "+" << std::endl;
  for (const auto &row : rows) {
    std::cout << "| " << std::left << setfill(' ') << setw(table_width) << row.first
              << " | " << setw(index_width) << row.second << " |" << std::endl;
  }
  std::cout << "+" << setfill('-') << setw(table_width + 2) << "" << "+"
            << setfill('-') << setw(index_width + 2) << "" << "+" << std::endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateIndex" << std::endl;
#endif
  if (context == nullptr) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  std::string index_name = ast->child_->val_;
  std::string table_name = ast->child_->next_->val_;
  std::vector<std::string> index_keys;
  std::string index_type = "bptree";
  pSyntaxNode node = ast->child_->next_->next_;
  while (node != nullptr) {
    if (node->type_ == kNodeColumnList) {
      pSyntaxNode key = node->child_;
      while (key != nullptr) {
        index_keys.emplace_back(key->val_);
        key = key->next_;
      }
    } else if (node->type_ == kNodeIndexType && node->child_ != nullptr) {
      index_type = NormalizeIndexType(node->child_->val_);
    }
    node = node->next_;
  }

  IndexInfo *index_info = nullptr;
  return context->GetCatalog()->CreateIndex(table_name, index_name, index_keys, context->GetTransaction(), index_info,
                                            index_type);
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropIndex" << std::endl;
#endif
  if (context == nullptr) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  std::string index_name = ast->child_->val_;
  std::vector<TableInfo *> tables;
  context->GetCatalog()->GetTables(tables);
  for (auto *table : tables) {
    IndexInfo *index_info = nullptr;
    if (context->GetCatalog()->GetIndex(table->GetTableName(), index_name, index_info) == DB_SUCCESS) {
      return context->GetCatalog()->DropIndex(table->GetTableName(), index_name);
    }
  }
  return DB_INDEX_NOT_FOUND;
}

dberr_t ExecuteEngine::ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxBegin" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxCommit" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxRollback" << std::endl;
#endif
  return DB_FAILED;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteExecfile" << std::endl;
#endif
  std::ifstream ifs(ast->child_->val_);
  if (!ifs.is_open()) {
    return DB_FAILED;
  }

  std::stringstream buffer;
  buffer << ifs.rdbuf();
  std::string content = buffer.str();
  std::string statement;
  for (char ch : content) {
    statement.push_back(ch);
    if (ch != ';') {
      continue;
    }
    YY_BUFFER_STATE bp = yy_scan_string(statement.c_str());
    if (bp == nullptr) {
      return DB_FAILED;
    }
    yy_switch_to_buffer(bp);
    MinisqlParserInit();
    yyparse();
    dberr_t result = DB_FAILED;
    if (!MinisqlParserGetError()) {
      result = Execute(MinisqlGetParserRootNode());
    }
    MinisqlParserFinish();
    yy_delete_buffer(bp);
    yylex_destroy();
    if (result != DB_SUCCESS) {
      return result;
    }
    statement.clear();
  }
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteQuit" << std::endl;
#endif
  return DB_QUIT;
}
