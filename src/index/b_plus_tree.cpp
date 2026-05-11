#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  if (leaf_max_size_ == UNDEFINED_SIZE) {
    leaf_max_size_ = static_cast<int>((PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + sizeof(RowId))) - 1;
  }
  if (internal_max_size_ == UNDEFINED_SIZE) {
    internal_max_size_ =
        static_cast<int>((PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + sizeof(page_id_t))) - 1;
  }

  if (!buffer_pool_manager_->IsPageFree(INDEX_ROOTS_PAGE_ID)) {
    auto *header_page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
    if (header_page != nullptr) {
      auto *roots_page = reinterpret_cast<IndexRootsPage *>(header_page->GetData());
      page_id_t stored_root_id;
      if (roots_page->GetRootId(index_id_, &stored_root_id)) {
        root_page_id_ = stored_root_id;
      }
      buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
    }
  }
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  if (current_page_id == INVALID_PAGE_ID) {
    if (IsEmpty()) {
      return;
    }
    current_page_id = root_page_id_;
  }

  auto *page = buffer_pool_manager_->FetchPage(current_page_id);
  if (page == nullptr) {
    return;
  }
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  if (!node->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(node);
    for (int i = 0; i < internal->GetSize(); i++) {
      Destroy(internal->ValueAt(i));
    }
  }
  buffer_pool_manager_->UnpinPage(current_page_id, false);
  buffer_pool_manager_->DeletePage(current_page_id);
  if (current_page_id == root_page_id_) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId();
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (IsEmpty()) {
    return false;
  }
  auto *page = FindLeafPage(key);
  if (page == nullptr) {
    return false;
  }
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  if (found) {
    result.emplace_back(value);
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  page_id_t root_page_id;
  auto *page = buffer_pool_manager_->NewPage(root_page_id);
  if (page == nullptr) {
    throw std::logic_error("Out of memory when creating root page.");
  }
  auto *root = reinterpret_cast<LeafPage *>(page->GetData());
  root->Init(root_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  root->Insert(key, value, processor_);
  root_page_id_ = root_page_id;
  UpdateRootPageId(1);
  buffer_pool_manager_->UnpinPage(root_page_id_, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  auto *page = FindLeafPage(key);
  if (page == nullptr) {
    return false;
  }
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  RowId existing;
  if (leaf->Lookup(key, existing, processor_)) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }

  leaf->Insert(key, value, processor_);
  if (leaf->GetSize() > leaf->GetMaxSize()) {
    auto *new_leaf = Split(leaf, transaction);
    InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, transaction);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
  page_id_t page_id;
  auto *page = buffer_pool_manager_->NewPage(page_id);
  if (page == nullptr) {
    throw std::logic_error("Out of memory when splitting internal page.");
  }
  auto *new_internal = reinterpret_cast<InternalPage *>(page->GetData());
  new_internal->Init(page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);
  node->MoveHalfTo(new_internal, buffer_pool_manager_);
  return new_internal;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
  page_id_t page_id;
  auto *page = buffer_pool_manager_->NewPage(page_id);
  if (page == nullptr) {
    throw std::logic_error("Out of memory when splitting leaf page.");
  }
  auto *new_leaf = reinterpret_cast<LeafPage *>(page->GetData());
  new_leaf->Init(page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);
  node->MoveHalfTo(new_leaf);
  new_leaf->SetNextPageId(node->GetNextPageId());
  node->SetNextPageId(new_leaf->GetPageId());
  return new_leaf;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node, Txn *transaction) {
  if (old_node->IsRootPage()) {
    page_id_t new_root_page_id;
    auto *page = buffer_pool_manager_->NewPage(new_root_page_id);
    if (page == nullptr) {
      throw std::logic_error("Out of memory when creating new root page.");
    }
    auto *root = reinterpret_cast<InternalPage *>(page->GetData());
    root->Init(new_root_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(new_root_page_id);
    new_node->SetParentPageId(new_root_page_id);
    root_page_id_ = new_root_page_id;
    UpdateRootPageId();
    buffer_pool_manager_->UnpinPage(new_root_page_id, true);
    return;
  }

  page_id_t parent_page_id = old_node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_page_id);
  ASSERT(parent_page != nullptr, "Parent page not found during insert.");
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  new_node->SetParentPageId(parent_page_id);
  parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
  if (parent->GetSize() > parent->GetMaxSize()) {
    auto *new_internal = Split(parent, transaction);
    InsertIntoParent(parent, new_internal->KeyAt(0), new_internal, transaction);
    buffer_pool_manager_->UnpinPage(new_internal->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(parent_page_id, true);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
  if (IsEmpty()) {
    return;
  }

  auto *page = FindLeafPage(key);
  if (page == nullptr) {
    return;
  }
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int old_size = leaf->GetSize();
  leaf->RemoveAndDeleteRecord(key, processor_);
  if (leaf->GetSize() == old_size) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return;
  }

  if (leaf->IsRootPage() || leaf->GetSize() < leaf->GetMinSize()) {
    CoalesceOrRedistribute(leaf, transaction);
    return;
  }
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }
  if (node->GetSize() >= node->GetMinSize()) {
    buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
    return false;
  }

  auto *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
  ASSERT(parent_page != nullptr, "Parent page not found during coalesce or redistribute.");
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  int index = parent->ValueIndex(node->GetPageId());
  int neighbor_index = (index == 0) ? 1 : index - 1;
  auto *neighbor_page = buffer_pool_manager_->FetchPage(parent->ValueAt(neighbor_index));
  ASSERT(neighbor_page != nullptr, "Neighbor page not found during coalesce or redistribute.");
  auto *neighbor_node = reinterpret_cast<N *>(neighbor_page->GetData());

  if (neighbor_node->GetSize() + node->GetSize() > node->GetMaxSize()) {
    Redistribute(neighbor_node, node, index);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return false;
  }

  bool node_deleted = Coalesce(neighbor_node, node, parent, index, transaction);
  return node_deleted;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  bool node_deleted = false;
  page_id_t deleted_page_id;
  page_id_t survivor_page_id;

  if (index == 0) {
    neighbor_node->MoveAllTo(node);
    parent->Remove(1);
    deleted_page_id = neighbor_node->GetPageId();
    survivor_page_id = node->GetPageId();
  } else {
    node->MoveAllTo(neighbor_node);
    parent->Remove(index);
    deleted_page_id = node->GetPageId();
    survivor_page_id = neighbor_node->GetPageId();
    node_deleted = true;
  }

  buffer_pool_manager_->UnpinPage(survivor_page_id, true);
  buffer_pool_manager_->UnpinPage(deleted_page_id, true);
  buffer_pool_manager_->DeletePage(deleted_page_id);

  if (parent->IsRootPage() || parent->GetSize() < parent->GetMinSize()) {
    CoalesceOrRedistribute(parent, transaction);
  } else {
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  }
  return node_deleted;
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  bool node_deleted = false;
  page_id_t deleted_page_id;
  page_id_t survivor_page_id;

  if (index == 0) {
    neighbor_node->MoveAllTo(node, parent->KeyAt(1), buffer_pool_manager_);
    parent->Remove(1);
    deleted_page_id = neighbor_node->GetPageId();
    survivor_page_id = node->GetPageId();
  } else {
    node->MoveAllTo(neighbor_node, parent->KeyAt(index), buffer_pool_manager_);
    parent->Remove(index);
    deleted_page_id = node->GetPageId();
    survivor_page_id = neighbor_node->GetPageId();
    node_deleted = true;
  }

  buffer_pool_manager_->UnpinPage(survivor_page_id, true);
  buffer_pool_manager_->UnpinPage(deleted_page_id, true);
  buffer_pool_manager_->DeletePage(deleted_page_id);

  if (parent->IsRootPage() || parent->GetSize() < parent->GetMinSize()) {
    CoalesceOrRedistribute(parent, transaction);
  } else {
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  }
  return node_deleted;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  auto *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
  ASSERT(parent_page != nullptr, "Parent page not found during leaf redistribute.");
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    neighbor_node->MoveLastToFrontOf(node);
    parent->SetKeyAt(index, node->KeyAt(0));
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  auto *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
  ASSERT(parent_page != nullptr, "Parent page not found during internal redistribute.");
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node, parent->KeyAt(1), buffer_pool_manager_);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    auto *middle_key = processor_.InitKey();
    memcpy(middle_key, neighbor_node->KeyAt(neighbor_node->GetSize() - 1), processor_.GetKeySize());
    neighbor_node->MoveLastToFrontOf(node, parent->KeyAt(index), buffer_pool_manager_);
    parent->SetKeyAt(index, middle_key);
    free(middle_key);
  }
  buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  if (old_root_node->IsLeafPage()) {
    if (old_root_node->GetSize() == 0) {
      page_id_t old_root_page_id = old_root_node->GetPageId();
      root_page_id_ = INVALID_PAGE_ID;
      buffer_pool_manager_->UnpinPage(old_root_page_id, true);
      buffer_pool_manager_->DeletePage(old_root_page_id);
      UpdateRootPageId();
      return true;
    }
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), true);
    return false;
  }

  auto *root = reinterpret_cast<InternalPage *>(old_root_node);
  if (root->GetSize() == 1) {
    page_id_t new_root_page_id = root->RemoveAndReturnOnlyChild();
    auto *new_root_page = buffer_pool_manager_->FetchPage(new_root_page_id);
    ASSERT(new_root_page != nullptr, "New root page not found.");
    auto *new_root = reinterpret_cast<BPlusTreePage *>(new_root_page->GetData());
    new_root->SetParentPageId(INVALID_PAGE_ID);
    root_page_id_ = new_root_page_id;
    buffer_pool_manager_->UnpinPage(new_root_page_id, true);
    page_id_t old_root_page_id = root->GetPageId();
    buffer_pool_manager_->UnpinPage(old_root_page_id, true);
    buffer_pool_manager_->DeletePage(old_root_page_id);
    UpdateRootPageId();
    return true;
  }
  buffer_pool_manager_->UnpinPage(root->GetPageId(), true);
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  if (IsEmpty()) {
    return End();
  }
  auto *page = FindLeafPage(nullptr, root_page_id_, true);
  if (page == nullptr) {
    return End();
  }
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  while (leaf->GetSize() == 0 && leaf->GetNextPageId() != INVALID_PAGE_ID) {
    page_id_t next_page_id = leaf->GetNextPageId();
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    auto *next_page = buffer_pool_manager_->FetchPage(next_page_id);
    leaf = reinterpret_cast<LeafPage *>(next_page->GetData());
  }
  if (leaf->GetSize() == 0) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return End();
  }
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, 0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) {
    return End();
  }
  auto *page = FindLeafPage(key);
  if (page == nullptr) {
    return End();
  }
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int index = leaf->KeyIndex(key, processor_);
  while ((leaf->GetSize() == 0 || index >= leaf->GetSize()) && leaf->GetNextPageId() != INVALID_PAGE_ID) {
    page_id_t next_page_id = leaf->GetNextPageId();
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    auto *next_page = buffer_pool_manager_->FetchPage(next_page_id);
    leaf = reinterpret_cast<LeafPage *>(next_page->GetData());
    index = 0;
  }
  if (leaf->GetSize() == 0 || index >= leaf->GetSize()) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return End();
  }
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, index);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  return IndexIterator(INVALID_PAGE_ID, buffer_pool_manager_, 0);
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (IsEmpty()) {
    return nullptr;
  }

  if (page_id == INVALID_PAGE_ID) {
    page_id = root_page_id_;
  }
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return nullptr;
  }

  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  while (!node->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(node);
    page_id_t next_page_id = leftMost ? internal->ValueAt(0) : internal->Lookup(key, processor_);
    buffer_pool_manager_->UnpinPage(internal->GetPageId(), false);
    page = buffer_pool_manager_->FetchPage(next_page_id);
    if (page == nullptr) {
      return nullptr;
    }
    node = reinterpret_cast<BPlusTreePage *>(page->GetData());
  }
  return page;
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
  auto *header_page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  if (header_page == nullptr) {
    return;
  }
  auto *roots_page = reinterpret_cast<IndexRootsPage *>(header_page->GetData());
  if (insert_record) {
    if (!roots_page->Insert(index_id_, root_page_id_)) {
      roots_page->Update(index_id_, root_page_id_);
    }
  } else if (root_page_id_ == INVALID_PAGE_ID) {
    roots_page->Delete(index_id_);
  } else if (!roots_page->Update(index_id_, root_page_id_)) {
    roots_page->Insert(index_id_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}
