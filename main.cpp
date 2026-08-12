#include <bits/stdc++.h>
#include <unistd.h>
using namespace std;

// External B+ tree keyed by (index string, int value).
// One file: "storage.db". Block size 4096 bytes.
// Memory usage is O(tree height * block size).

static const char DB_FILE[] = "storage.db";
static const uint32_t SUPER_BLOCK = 0;
static const int BLOCK_SIZE = 16384;
static const int NODE_DATA_OFF = 16;
static const int MAX_RECS = 220;   // leaf capacity
static const int MAX_KEYS = 220;   // internal key capacity

static FILE *db = nullptr;

struct Rec {
    uint8_t klen;
    char key[64];
    int32_t val;
} __attribute__((packed));
static_assert(sizeof(Rec) == 69, "Rec size mismatch");

static inline int rec_cmp(const Rec &a, const Rec &b) {
    int m = a.klen < b.klen ? a.klen : b.klen;
    int c = memcmp(a.key, b.key, m);
    if (c != 0) return c;
    if (a.klen != b.klen) return a.klen < b.klen ? -1 : 1;
    if (a.val != b.val) return a.val < b.val ? -1 : 1;
    return 0;
}

static inline Rec make_rec(const string &s, int32_t v) {
    Rec r;
    r.klen = (uint8_t)s.size();
    memset(r.key, 0, 64);
    memcpy(r.key, s.data(), r.klen);
    r.val = v;
    return r;
}

static inline Rec key_lower_bound(const string &s) {
    Rec r;
    r.klen = (uint8_t)s.size();
    memset(r.key, 0, 64);
    memcpy(r.key, s.data(), r.klen);
    r.val = INT32_MIN;
    return r;
}

static inline int key_cmp_str(const Rec &a, const string &s) {
    int slen = (int)s.size();
    int m = a.klen < slen ? a.klen : slen;
    int c = memcmp(a.key, s.data(), m);
    if (c != 0) return c;
    if (a.klen != slen) return a.klen < slen ? -1 : 1;
    return 0;
}

static inline void read_block(uint32_t id, char *buf) {
    fseek(db, (long)id * BLOCK_SIZE, SEEK_SET);
    size_t got = fread(buf, 1, BLOCK_SIZE, db);
    if (got < BLOCK_SIZE) memset(buf + got, 0, BLOCK_SIZE - got);
}

static inline void write_block(uint32_t id, const char *buf) {
    fseek(db, (long)id * BLOCK_SIZE, SEEK_SET);
    fwrite(buf, 1, BLOCK_SIZE, db);
}

static inline uint8_t  nd_leaf(const char *b)   { return (uint8_t)b[0]; }
static inline uint16_t nd_count(const char *b)  { uint16_t v; memcpy(&v, b + 1, 2); return v; }
static inline uint32_t nd_parent(const char *b) { uint32_t v; memcpy(&v, b + 3, 4); return v; }
static inline uint32_t nd_next(const char *b)   { uint32_t v; memcpy(&v, b + 7, 4); return v; }

static inline void nd_set_leaf(char *b, uint8_t v)    { b[0] = (char)v; }
static inline void nd_set_count(char *b, uint16_t v)  { memcpy(b + 1, &v, 2); }
static inline void nd_set_parent(char *b, uint32_t v) { memcpy(b + 3, &v, 4); }
static inline void nd_set_next(char *b, uint32_t v)   { memcpy(b + 7, &v, 4); }

static inline void nd_init_leaf(char *b) {
    memset(b, 0, BLOCK_SIZE);
    nd_set_leaf(b, 1);
    nd_set_count(b, 0);
}
static inline void nd_init_internal(char *b) {
    memset(b, 0, BLOCK_SIZE);
    nd_set_leaf(b, 0);
    nd_set_count(b, 0);
}

static inline Rec nd_leaf_get(const char *b, int i) {
    Rec r; memcpy(&r, b + NODE_DATA_OFF + i * sizeof(Rec), sizeof(Rec)); return r;
}
static inline void nd_leaf_set(char *b, int i, const Rec &r) {
    memcpy(b + NODE_DATA_OFF + i * sizeof(Rec), &r, sizeof(Rec));
}

static inline uint32_t nd_int_child(const char *b, int i) {
    if (i == 0) {
        uint32_t v; memcpy(&v, b + NODE_DATA_OFF, 4); return v;
    }
    int off = NODE_DATA_OFF + 4 + (i - 1) * (sizeof(Rec) + 4) + sizeof(Rec);
    uint32_t v; memcpy(&v, b + off, 4); return v;
}
static inline void nd_int_set_child(char *b, int i, uint32_t id) {
    if (i == 0) {
        memcpy(b + NODE_DATA_OFF, &id, 4); return;
    }
    int off = NODE_DATA_OFF + 4 + (i - 1) * (sizeof(Rec) + 4) + sizeof(Rec);
    memcpy(b + off, &id, 4);
}
static inline Rec nd_int_key(const char *b, int i) {
    int off = NODE_DATA_OFF + 4 + i * (sizeof(Rec) + 4);
    Rec r; memcpy(&r, b + off, sizeof(Rec)); return r;
}
static inline void nd_int_set_key(char *b, int i, const Rec &r) {
    int off = NODE_DATA_OFF + 4 + i * (sizeof(Rec) + 4);
    memcpy(b + off, &r, sizeof(Rec));
}

static uint32_t get_root() {
    char buf[BLOCK_SIZE];
    read_block(SUPER_BLOCK, buf);
    uint32_t r; memcpy(&r, buf, 4); return r;
}
static void set_root(uint32_t r) {
    char buf[BLOCK_SIZE];
    read_block(SUPER_BLOCK, buf);
    memcpy(buf, &r, 4);
    write_block(SUPER_BLOCK, buf);
}
static uint32_t get_next_free() {
    char buf[BLOCK_SIZE];
    read_block(SUPER_BLOCK, buf);
    uint32_t v; memcpy(&v, buf + 4, 4); return v;
}
static void set_next_free(uint32_t v) {
    char buf[BLOCK_SIZE];
    read_block(SUPER_BLOCK, buf);
    memcpy(buf + 4, &v, 4);
    write_block(SUPER_BLOCK, buf);
}
static uint32_t alloc_block() {
    uint32_t id = get_next_free();
    set_next_free(id + 1);
    // make sure file is long enough (write an empty block)
    char empty[BLOCK_SIZE] = {0};
    write_block(id, empty);
    return id;
}

static void init_db() {
    char sb[BLOCK_SIZE] = {0};
    uint32_t root_id = 1;
    uint32_t next_free = 2;
    memcpy(sb, &root_id, 4);
    memcpy(sb + 4, &next_free, 4);
    write_block(SUPER_BLOCK, sb);
    char root[BLOCK_SIZE];
    nd_init_leaf(root);
    nd_set_parent(root, 0);
    nd_set_next(root, 0);
    write_block(root_id, root);
}

static uint32_t find_leaf(const Rec &target) {
    char buf[BLOCK_SIZE];
    uint32_t cur = get_root();
    while (true) {
        read_block(cur, buf);
        if (nd_leaf(buf)) return cur;
        int cnt = nd_count(buf);
        uint32_t child = nd_int_child(buf, 0);
        for (int i = 0; i < cnt; ++i) {
            Rec k = nd_int_key(buf, i);
            if (rec_cmp(target, k) < 0) break;
            child = nd_int_child(buf, i + 1);
        }
        cur = child;
    }
}

static void insert_into_parent(uint32_t parent_id, uint32_t left_id, uint32_t right_id, const Rec &sep);

static void insert_rec(const Rec &r) {
    uint32_t leaf_id = find_leaf(r);
    char buf[BLOCK_SIZE];
    read_block(leaf_id, buf);
    int cnt = nd_count(buf);
    int pos = 0;
    while (pos < cnt && rec_cmp(nd_leaf_get(buf, pos), r) < 0) ++pos;
    if (pos < cnt && rec_cmp(nd_leaf_get(buf, pos), r) == 0) return; // already exists

    if (cnt < MAX_RECS) {
        for (int i = cnt - 1; i >= pos; --i) nd_leaf_set(buf, i + 1, nd_leaf_get(buf, i));
        nd_leaf_set(buf, pos, r);
        nd_set_count(buf, cnt + 1);
        write_block(leaf_id, buf);
        return;
    }

    // split leaf
    Rec temp[MAX_RECS + 1];
    for (int i = 0; i < pos; ++i) temp[i] = nd_leaf_get(buf, i);
    temp[pos] = r;
    for (int i = pos; i < cnt; ++i) temp[i + 1] = nd_leaf_get(buf, i);

    int total = cnt + 1;
    int left_cnt = total / 2 + (total % 2);
    int right_cnt = total - left_cnt;

    uint32_t new_id = alloc_block();
    char newbuf[BLOCK_SIZE];
    nd_init_leaf(newbuf);
    nd_set_parent(newbuf, nd_parent(buf));
    nd_set_next(newbuf, nd_next(buf));

    for (int i = 0; i < left_cnt; ++i) nd_leaf_set(buf, i, temp[i]);
    nd_set_count(buf, left_cnt);
    nd_set_next(buf, new_id);

    for (int i = 0; i < right_cnt; ++i) nd_leaf_set(newbuf, i, temp[left_cnt + i]);
    nd_set_count(newbuf, right_cnt);

    Rec sep = temp[left_cnt];
    write_block(leaf_id, buf);
    write_block(new_id, newbuf);

    uint32_t parent = nd_parent(buf);
    if (parent == 0) {
        uint32_t new_root = alloc_block();
        char rootb[BLOCK_SIZE];
        nd_init_internal(rootb);
        nd_set_count(rootb, 1);
        nd_int_set_child(rootb, 0, leaf_id);
        nd_int_set_key(rootb, 0, sep);
        nd_int_set_child(rootb, 1, new_id);
        write_block(new_root, rootb);
        nd_set_parent(buf, new_root);
        nd_set_parent(newbuf, new_root);
        write_block(leaf_id, buf);
        write_block(new_id, newbuf);
        set_root(new_root);
    } else {
        insert_into_parent(parent, leaf_id, new_id, sep);
    }
}

static void insert_into_parent(uint32_t parent_id, uint32_t left_id, uint32_t right_id, const Rec &sep) {
    char buf[BLOCK_SIZE];
    read_block(parent_id, buf);
    int cnt = nd_count(buf);
    int left_pos = 0;
    while (left_pos <= cnt && nd_int_child(buf, left_pos) != left_id) ++left_pos;
    // keys before left_pos are 0..left_pos-1
    if (cnt < MAX_KEYS) {
        // shift keys left_pos..cnt-1 to left_pos+1
        for (int i = cnt - 1; i >= left_pos; --i) nd_int_set_key(buf, i + 1, nd_int_key(buf, i));
        // shift children left_pos+1..cnt to left_pos+2
        for (int i = cnt; i >= left_pos + 1; --i) nd_int_set_child(buf, i + 1, nd_int_child(buf, i));
        nd_int_set_key(buf, left_pos, sep);
        nd_int_set_child(buf, left_pos + 1, right_id);
        nd_set_count(buf, cnt + 1);
        write_block(parent_id, buf);
        return;
    }

    // split internal node
    Rec keys[MAX_KEYS + 1];
    uint32_t children[MAX_KEYS + 2];
    uint32_t old_parent = nd_parent(buf);
    children[0] = nd_int_child(buf, 0);
    for (int i = 0; i < cnt; ++i) {
        keys[i] = nd_int_key(buf, i);
        children[i + 1] = nd_int_child(buf, i + 1);
    }
    // insert sep and right_id at left_pos
    for (int i = cnt - 1; i >= left_pos; --i) keys[i + 1] = keys[i];
    for (int i = cnt; i >= left_pos + 1; --i) children[i + 1] = children[i];
    keys[left_pos] = sep;
    children[left_pos + 1] = right_id;
    int total_children = cnt + 2; // = MAX_KEYS + 2
    int mid = total_children / 2; // children index of first child of right node

    // left node keeps children[0..mid-1] and keys[0..mid-2]
    nd_init_internal(buf);
    nd_set_parent(buf, old_parent);
    nd_set_count(buf, mid - 1);
    nd_int_set_child(buf, 0, children[0]);
    for (int i = 0; i < mid - 1; ++i) {
        nd_int_set_key(buf, i, keys[i]);
        nd_int_set_child(buf, i + 1, children[i + 1]);
    }
    // key at mid-1 is the separator between left and right; promote it
    Rec promoted = keys[mid - 1];

    uint32_t right_node = alloc_block();
    char rbuf[BLOCK_SIZE];
    nd_init_internal(rbuf);
    nd_set_parent(rbuf, old_parent);
    int right_cnt = total_children - mid - 1; // keys count in right
    nd_set_count(rbuf, right_cnt);
    nd_int_set_child(rbuf, 0, children[mid]);
    for (int i = 0; i < right_cnt; ++i) {
        nd_int_set_key(rbuf, i, keys[mid + i]);
        nd_int_set_child(rbuf, i + 1, children[mid + i + 1]);
    }

    // update parent pointers of children moved to the right node
    char childbuf[BLOCK_SIZE];
    for (int i = 0; i <= right_cnt; ++i) {
        uint32_t cid = nd_int_child(rbuf, i);
        read_block(cid, childbuf);
        nd_set_parent(childbuf, right_node);
        write_block(cid, childbuf);
    }

    write_block(parent_id, buf);
    write_block(right_node, rbuf);

    uint32_t grand = old_parent;
    if (grand == 0) {
        uint32_t new_root = alloc_block();
        char rootb[BLOCK_SIZE];
        nd_init_internal(rootb);
        nd_set_count(rootb, 1);
        nd_int_set_child(rootb, 0, parent_id);
        nd_int_set_key(rootb, 0, promoted);
        nd_int_set_child(rootb, 1, right_node);
        write_block(new_root, rootb);
        nd_set_parent(buf, new_root);
        nd_set_parent(rbuf, new_root);
        write_block(parent_id, buf);
        write_block(right_node, rbuf);
        set_root(new_root);
    } else {
        // ensure right_node's parent pointer is correct before recursive call (will be set by parent)
        nd_set_parent(rbuf, grand);
        write_block(right_node, rbuf);
        insert_into_parent(grand, parent_id, right_node, promoted);
    }
}

static void delete_rec(const Rec &r) {
    uint32_t leaf_id = find_leaf(r);
    char buf[BLOCK_SIZE];
    read_block(leaf_id, buf);
    int cnt = nd_count(buf);
    int pos = -1;
    for (int i = 0; i < cnt; ++i) {
        if (rec_cmp(nd_leaf_get(buf, i), r) == 0) { pos = i; break; }
    }
    if (pos < 0) return;
    for (int i = pos; i < cnt - 1; ++i) nd_leaf_set(buf, i, nd_leaf_get(buf, i + 1));
    nd_set_count(buf, cnt - 1);
    write_block(leaf_id, buf);
}

static void find_key(const string &key, ostream &out) {
    Rec target = key_lower_bound(key);
    uint32_t leaf_id = find_leaf(target);
    char buf[BLOCK_SIZE];
    vector<int32_t> vals;
    uint32_t cur = leaf_id;
    while (cur != 0) {
        read_block(cur, buf);
        int cnt = nd_count(buf);
        if (cnt == 0) {
            cur = nd_next(buf);
            continue;
        }
        Rec first = nd_leaf_get(buf, 0);
        Rec last = nd_leaf_get(buf, cnt - 1);
        if (key_cmp_str(first, key) > 0) break; // gone past the key
        if (key_cmp_str(last, key) < 0) {
            cur = nd_next(buf);
            continue;
        }
        bool any = false;
        for (int i = 0; i < cnt; ++i) {
            Rec r = nd_leaf_get(buf, i);
            int c = key_cmp_str(r, key);
            if (c == 0) {
                any = true;
                vals.push_back(r.val);
            } else if (c > 0) {
                break;
            }
        }
        if (!any) break; // should not happen if bounds check passed, but safety
        if (key_cmp_str(last, key) == 0) {
            cur = nd_next(buf);
        } else {
            break;
        }
    }
    if (vals.empty()) {
        out << "null\n";
    } else {
        for (size_t i = 0; i < vals.size(); ++i) {
            if (i) out << ' ';
            out << vals[i];
        }
        out << '\n';
    }
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    bool exists = (access(DB_FILE, F_OK) == 0);
    db = fopen(DB_FILE, "r+b");
    if (!db) db = fopen(DB_FILE, "w+b");
    if (!exists) init_db();

    int n;
    if (!(cin >> n)) return 0;
    string cmd;
    for (int i = 0; i < n; ++i) {
        cin >> cmd;
        if (cmd == "insert") {
            string idx; int v; cin >> idx >> v;
            insert_rec(make_rec(idx, (int32_t)v));
        } else if (cmd == "delete") {
            string idx; int v; cin >> idx >> v;
            delete_rec(make_rec(idx, (int32_t)v));
        } else if (cmd == "find") {
            string idx; cin >> idx;
            find_key(idx, cout);
        }
    }
    fflush(db);
    fclose(db);
    return 0;
}
