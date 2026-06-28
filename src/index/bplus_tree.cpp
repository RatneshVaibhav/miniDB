#include "index/bplus_tree.h"

#include <cstring>

#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace minidb {

// Physical capacities allow one temporary overflow entry before a split.
static constexpr int LEAF_CAP = BPlusTree::LEAF_MAX + 1;       // 101
static constexpr int INT_CAP = BPlusTree::INTERNAL_MAX + 1;    // 111

static constexpr int IS_LEAF_OFF = 8;
static constexpr int SIZE_OFF = 12;
static constexpr int NEXT_OFF = 16;
static constexpr int DATA_OFF = 24;

// ---- raw-page accessors ----
namespace {
struct Node {
  char *d;
  explicit Node(char *data) : d(data) {}

  bool is_leaf() const { uint8_t b; std::memcpy(&b, d + IS_LEAF_OFF, 1); return b != 0; }
  void set_leaf(bool v) { uint8_t b = v ? 1 : 0; std::memcpy(d + IS_LEAF_OFF, &b, 1); }
  int size() const { uint32_t s; std::memcpy(&s, d + SIZE_OFF, 4); return static_cast<int>(s); }
  void set_size(int s) { uint32_t v = static_cast<uint32_t>(s); std::memcpy(d + SIZE_OFF, &v, 4); }
  page_id_t next() const { page_id_t p; std::memcpy(&p, d + NEXT_OFF, 4); return p; }
  void set_next(page_id_t p) { std::memcpy(d + NEXT_OFF, &p, 4); }

  // leaf key/rid regions
  IndexKey leaf_key(int i) const { IndexKey k; std::memcpy(k.data, d + DATA_OFF + i * IndexKey::LEN, IndexKey::LEN); return k; }
  void set_leaf_key(int i, const IndexKey &k) { std::memcpy(d + DATA_OFF + i * IndexKey::LEN, k.data, IndexKey::LEN); }
  RID leaf_rid(int i) const {
    RID r; int base = DATA_OFF + LEAF_CAP * IndexKey::LEN + i * 8;
    std::memcpy(&r.page_id, d + base, 4); std::memcpy(&r.slot, d + base + 4, 4); return r;
  }
  void set_leaf_rid(int i, const RID &r) {
    int base = DATA_OFF + LEAF_CAP * IndexKey::LEN + i * 8;
    std::memcpy(d + base, &r.page_id, 4); std::memcpy(d + base + 4, &r.slot, 4);
  }

  // internal child/key regions
  page_id_t child(int i) const { page_id_t p; std::memcpy(&p, d + DATA_OFF + i * 4, 4); return p; }
  void set_child(int i, page_id_t p) { std::memcpy(d + DATA_OFF + i * 4, &p, 4); }
  IndexKey int_key(int i) const {
    IndexKey k; std::memcpy(k.data, d + DATA_OFF + INT_CAP * 4 + i * IndexKey::LEN, IndexKey::LEN); return k;
  }
  void set_int_key(int i, const IndexKey &k) {
    std::memcpy(d + DATA_OFF + INT_CAP * 4 + i * IndexKey::LEN, k.data, IndexKey::LEN);
  }
};
}  // namespace

bool BPlusTree::Search(const IndexKey &key, RID *out) const {
  if (empty()) return false;
  page_id_t leaf_id = FindLeaf(key);
  Page *page = bpm_->FetchPage(leaf_id);
  Node n(page->GetData());
  bool found = false;
  for (int i = 0; i < n.size(); i++) {
    if (n.leaf_key(i) == key) { *out = n.leaf_rid(i); found = true; break; }
  }
  bpm_->UnpinPage(leaf_id, false);
  return found;
}

page_id_t BPlusTree::FindLeaf(const IndexKey &key) const {
  page_id_t cur = root_page_id_;
  while (true) {
    Page *page = bpm_->FetchPage(cur);
    Node n(page->GetData());
    if (n.is_leaf()) { bpm_->UnpinPage(cur, false); return cur; }
    // choose child: first key strictly greater than `key`
    int i = 0;
    while (i < n.size() && key.Compare(n.int_key(i)) >= 0) i++;
    page_id_t child = n.child(i);
    bpm_->UnpinPage(cur, false);
    cur = child;
  }
}

// Descend taking the LEFT branch on equality so we land at (or before) the
// first leaf that can contain `key`; a forward scan then sees all duplicates.
page_id_t BPlusTree::FindLeafForScan(const IndexKey &key) const {
  page_id_t cur = root_page_id_;
  while (true) {
    Page *page = bpm_->FetchPage(cur);
    Node n(page->GetData());
    if (n.is_leaf()) { bpm_->UnpinPage(cur, false); return cur; }
    int i = 0;
    while (i < n.size() && key.Compare(n.int_key(i)) > 0) i++;
    page_id_t child = n.child(i);
    bpm_->UnpinPage(cur, false);
    cur = child;
  }
}

bool BPlusTree::Insert(const IndexKey &key, const RID &rid) {
  if (empty()) {
    page_id_t pid;
    Page *page = bpm_->NewPage(&pid);
    Node n(page->GetData());
    n.set_leaf(true);
    n.set_size(1);
    n.set_next(INVALID_PAGE_ID);
    n.set_leaf_key(0, key);
    n.set_leaf_rid(0, rid);
    bpm_->UnpinPage(pid, true);
    root_page_id_ = pid;
    return true;
  }
  bool inserted = true;
  SplitResult sr = InsertInto(root_page_id_, key, rid, &inserted);
  if (!inserted) return false;
  if (sr.split) StartNewRoot(sr.up_key, root_page_id_, sr.new_page);
  return true;
}

BPlusTree::SplitResult BPlusTree::InsertInto(page_id_t node_id, const IndexKey &key,
                                             const RID &rid, bool *inserted) {
  Page *page = bpm_->FetchPage(node_id);
  Node n(page->GetData());

  if (n.is_leaf()) {
    int s = n.size();
    int pos = 0;
    while (pos < s && n.leaf_key(pos).Compare(key) < 0) pos++;
    if (unique_ && pos < s && n.leaf_key(pos) == key) {  // duplicate in a unique index
      *inserted = false;
      bpm_->UnpinPage(node_id, false);
      return {};
    }
    for (int i = s; i > pos; i--) { n.set_leaf_key(i, n.leaf_key(i - 1)); n.set_leaf_rid(i, n.leaf_rid(i - 1)); }
    n.set_leaf_key(pos, key);
    n.set_leaf_rid(pos, rid);
    n.set_size(s + 1);

    if (n.size() <= LEAF_MAX) { bpm_->UnpinPage(node_id, true); return {}; }

    // split leaf
    int total = n.size();
    int mid = total / 2;          // right gets [mid, total)
    page_id_t rpid;
    Page *rpage = bpm_->NewPage(&rpid);
    Node r(rpage->GetData());
    r.set_leaf(true);
    int rcount = total - mid;
    for (int i = 0; i < rcount; i++) { r.set_leaf_key(i, n.leaf_key(mid + i)); r.set_leaf_rid(i, n.leaf_rid(mid + i)); }
    r.set_size(rcount);
    r.set_next(n.next());
    n.set_size(mid);
    n.set_next(rpid);
    SplitResult sr{true, r.leaf_key(0), rpid};
    bpm_->UnpinPage(rpid, true);
    bpm_->UnpinPage(node_id, true);
    return sr;
  }

  // internal: descend
  int i = 0;
  while (i < n.size() && key.Compare(n.int_key(i)) >= 0) i++;
  page_id_t child_id = n.child(i);
  SplitResult child_sr = InsertInto(child_id, key, rid, inserted);
  if (!*inserted || !child_sr.split) {  // nothing to merge into this node
    bpm_->UnpinPage(node_id, false);
    return {};
  }

  // insert separator (child_sr.up_key, child_sr.new_page) at position i
  int s = n.size();
  for (int j = s; j > i; j--) n.set_int_key(j, n.int_key(j - 1));
  for (int j = s + 1; j > i + 1; j--) n.set_child(j, n.child(j - 1));
  n.set_int_key(i, child_sr.up_key);
  n.set_child(i + 1, child_sr.new_page);
  n.set_size(s + 1);

  if (n.size() <= INTERNAL_MAX) { bpm_->UnpinPage(node_id, true); return {}; }

  // split internal: middle key moves up
  int total = n.size();
  int mid = total / 2;                 // key[mid] goes up
  IndexKey up = n.int_key(mid);
  page_id_t rpid;
  Page *rpage = bpm_->NewPage(&rpid);
  Node r(rpage->GetData());
  r.set_leaf(false);
  int rkeys = total - mid - 1;
  for (int k = 0; k < rkeys; k++) r.set_int_key(k, n.int_key(mid + 1 + k));
  for (int k = 0; k <= rkeys; k++) r.set_child(k, n.child(mid + 1 + k));
  r.set_size(rkeys);
  n.set_size(mid);
  SplitResult sr{true, up, rpid};
  bpm_->UnpinPage(rpid, true);
  bpm_->UnpinPage(node_id, true);
  return sr;
}

void BPlusTree::StartNewRoot(const IndexKey &key, page_id_t left, page_id_t right) {
  page_id_t pid;
  Page *page = bpm_->NewPage(&pid);
  Node n(page->GetData());
  n.set_leaf(false);
  n.set_size(1);
  n.set_child(0, left);
  n.set_int_key(0, key);
  n.set_child(1, right);
  bpm_->UnpinPage(pid, true);
  root_page_id_ = pid;
}

bool BPlusTree::Remove(const IndexKey &key) {
  if (empty()) return false;
  page_id_t leaf_id = FindLeaf(key);
  Page *page = bpm_->FetchPage(leaf_id);
  Node n(page->GetData());
  int s = n.size();
  int pos = -1;
  for (int i = 0; i < s; i++) { if (n.leaf_key(i) == key) { pos = i; break; } }
  if (pos < 0) { bpm_->UnpinPage(leaf_id, false); return false; }
  for (int i = pos; i < s - 1; i++) { n.set_leaf_key(i, n.leaf_key(i + 1)); n.set_leaf_rid(i, n.leaf_rid(i + 1)); }
  n.set_size(s - 1);  // underflow tolerated (no merge); search/scan stay correct
  bpm_->UnpinPage(leaf_id, true);
  return true;
}

bool BPlusTree::Remove(const IndexKey &key, const RID &rid) {
  if (empty()) return false;
  // Walk equal-key entries (possibly across linked leaves) for the exact rid.
  page_id_t cur = FindLeafForScan(key);
  while (cur != INVALID_PAGE_ID) {
    Page *page = bpm_->FetchPage(cur);
    Node n(page->GetData());
    bool done = false;
    for (int i = 0; i < n.size(); i++) {
      int c = n.leaf_key(i).Compare(key);
      if (c < 0) continue;
      if (c > 0) { done = true; break; }       // past the key range
      if (n.leaf_rid(i) == rid) {
        for (int j = i; j < n.size() - 1; j++) { n.set_leaf_key(j, n.leaf_key(j + 1)); n.set_leaf_rid(j, n.leaf_rid(j + 1)); }
        n.set_size(n.size() - 1);
        bpm_->UnpinPage(cur, true);
        return true;
      }
    }
    page_id_t next = n.next();
    bpm_->UnpinPage(cur, false);
    if (done) break;
    cur = next;
  }
  return false;
}

void BPlusTree::RangeScan(const IndexKey &low, const IndexKey &high,
                          const std::function<void(const IndexKey &, const RID &)> &cb) const {
  if (empty()) return;
  page_id_t cur = FindLeafForScan(low);  // leftmost leaf so duplicates aren't skipped
  while (cur != INVALID_PAGE_ID) {
    Page *page = bpm_->FetchPage(cur);
    Node n(page->GetData());
    bool done = false;
    for (int i = 0; i < n.size(); i++) {
      IndexKey k = n.leaf_key(i);
      if (k.Compare(low) < 0) continue;
      if (k.Compare(high) > 0) { done = true; break; }
      cb(k, n.leaf_rid(i));
    }
    page_id_t next = n.next();
    bpm_->UnpinPage(cur, false);
    if (done) break;
    cur = next;
  }
}

}  // namespace minidb
