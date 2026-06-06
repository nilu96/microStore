/*
 * Unit tests for microStore::TypedTieredStore write-back cache behaviour.
 */

#include <unity.h>
#include <microStore/HeapStore.h>
#include <microStore/TypedTieredStore.h>

#include <cstring>
#include <map>
#include <string>

class CountingStore {
public:
    using Entry = microStore::HeapStore::Entry;
    using iterator = microStore::HeapStore::iterator;

    bool init(bool clearOnInit = false) { return heap.init(clearOnInit); }
    bool isValid() const { return heap.isValid(); }
    operator bool() const { return isValid(); }
    void close() { heap.close(); }
    void clear() { heap.clear(); put_count = 0; remove_count = 0; }

    bool put(const uint8_t* key, uint8_t key_len, const void* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time(), uint8_t priority = 0) {
        put_count++;
        last_ttl = ttl;
        last_priority = priority;
        return heap.put(key, key_len, data, len, ttl, ts);
    }
    bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint32_t ttl = 0, uint32_t ts = microStore::time(), uint8_t priority = 0) {
        return put(key.data(), (uint8_t)key.size(), data.data(), (uint16_t)data.size(), ttl, ts, priority);
    }

    bool get(const uint8_t* key, uint8_t key_len, void* out, uint16_t* size) {
        return heap.get(key, key_len, out, size);
    }
    bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out) {
        return heap.get(key, out);
    }

    bool remove(const uint8_t* key, uint8_t key_len) {
        remove_count++;
        return heap.remove(key, key_len);
    }
    bool remove(const std::vector<uint8_t>& key) {
        return remove(key.data(), (uint8_t)key.size());
    }

    bool exists(const std::vector<uint8_t>& key) { return heap.exists(key); }

    size_t size() const { return heap.size(); }
    void set_ttl_secs(uint32_t ttl_s) { heap.set_ttl_secs(ttl_s); }
    void set_max_recs(uint32_t max_recs) { heap.set_max_recs(max_recs); }

    iterator begin() { return heap.begin(); }
    iterator end() { return heap.end(); }

    int put_count = 0;
    int remove_count = 0;
    uint32_t last_ttl = 0;
    uint8_t last_priority = 0;
    microStore::HeapStore heap;
};

using StringTieredStore = microStore::TypedTieredStore<std::string, std::string, std::hash<std::string>, CountingStore>;

static std::vector<uint8_t> bytes(const char* s) {
    return std::vector<uint8_t>((const uint8_t*)s, (const uint8_t*)s + strlen(s));
}

static std::string read_l2_string(CountingStore& store, const char* key) {
    std::vector<uint8_t> raw;
    if (!store.get(bytes(key), raw)) return "";
    return std::string(raw.begin(), raw.end());
}

void test_typed_tiered_store_defers_put_until_sync() {
    CountingStore l2;
    l2.init();
    StringTieredStore store(l2);

    TEST_ASSERT_TRUE(store.put("a", "one", 0, 0));
    TEST_ASSERT_EQUAL(0, l2.put_count);
    TEST_ASSERT_FALSE(l2.exists(bytes("a")));
    TEST_ASSERT_EQUAL(1u, store.dirtyCount());

    std::string out;
    TEST_ASSERT_TRUE(store.get("a", out));
    TEST_ASSERT_EQUAL_STRING("one", out.c_str());

    TEST_ASSERT_TRUE(store.sync());
    TEST_ASSERT_EQUAL(1, l2.put_count);
    TEST_ASSERT_TRUE(l2.exists(bytes("a")));
    TEST_ASSERT_EQUAL(0u, store.dirtyCount());
}

void test_typed_tiered_store_evicts_lru_dirty_tail() {
    CountingStore l2;
    l2.init();
    StringTieredStore store(l2);

    for (int i = 0; i < USTORE_L1_CACHE_SIZE; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        TEST_ASSERT_TRUE(store.put(key, "v", 0, 0));
    }

    std::string out;
    TEST_ASSERT_TRUE(store.get("k0", out)); // k0 becomes MRU, k1 is now older than the rest.

    TEST_ASSERT_TRUE(store.put("new", "n", 0, 0));
    TEST_ASSERT_EQUAL(1, l2.put_count);
    TEST_ASSERT_TRUE(l2.exists(bytes("k1")));
    TEST_ASSERT_FALSE(l2.exists(bytes("k0")));
    TEST_ASSERT_FALSE(l2.exists(bytes("new")));
    TEST_ASSERT_EQUAL((size_t)USTORE_L1_CACHE_SIZE, store.cacheSize());
}

void test_typed_tiered_store_sync_flushes_all_dirty_entries() {
    CountingStore l2;
    l2.init();
    StringTieredStore store(l2);

    store.put("a", "1", 0, 0);
    store.put("b", "2", 0, 0);
    store.put("c", "3", 0, 0);

    TEST_ASSERT_EQUAL(0, l2.put_count);
    TEST_ASSERT_TRUE(store.sync());
    TEST_ASSERT_EQUAL(3, l2.put_count);
    TEST_ASSERT_EQUAL(0u, store.dirtyCount());
    TEST_ASSERT_TRUE(l2.exists(bytes("a")));
    TEST_ASSERT_TRUE(l2.exists(bytes("b")));
    TEST_ASSERT_TRUE(l2.exists(bytes("c")));
}

void test_typed_tiered_store_flushes_remaining_ttl() {
    CountingStore l2;
    l2.init();
    StringTieredStore store(l2);

    TEST_ASSERT_TRUE(store.put("ttl", "value", 120, 0));
    TEST_ASSERT_TRUE(store.sync());
    TEST_ASSERT_GREATER_THAN(0u, l2.last_ttl);
    TEST_ASSERT_LESS_OR_EQUAL(120u, l2.last_ttl);
}

void test_typed_tiered_store_flushes_priority() {
    CountingStore l2;
    l2.init();
    StringTieredStore store(l2);

    TEST_ASSERT_TRUE(store.put("prio", "value", 0, 7));
    TEST_ASSERT_TRUE(store.sync());
    TEST_ASSERT_EQUAL_UINT8(7, l2.last_priority);
}

void test_typed_tiered_store_uses_l2_store_for_existing_keys() {
    CountingStore l2;
    l2.init();
    l2.put(bytes("flash"), bytes("F"));
    l2.put(bytes("other"), bytes("O"));
    l2.put_count = 0;

    StringTieredStore store(l2);

    TEST_ASSERT_TRUE(store.exists("flash"));
    TEST_ASSERT_TRUE(store.exists("other"));
    TEST_ASSERT_EQUAL(2u, store.size());
    TEST_ASSERT_EQUAL(0, l2.put_count);
}

void test_typed_tiered_store_promoted_l2_entry_is_not_double_counted() {
    CountingStore l2;
    l2.init();
    l2.put(bytes("flash"), bytes("F"));
    l2.put_count = 0;

    StringTieredStore store(l2);
    TEST_ASSERT_EQUAL(1u, store.size());

    std::string out;
    TEST_ASSERT_TRUE(store.get("flash", out));
    TEST_ASSERT_EQUAL_STRING("F", out.c_str());

    TEST_ASSERT_EQUAL(1u, store.cacheSize());
    TEST_ASSERT_EQUAL(1u, store.size());
}

void test_typed_tiered_store_clean_l2_entry_remains_visible_after_eviction() {
    CountingStore l2;
    l2.init();
    l2.put(bytes("flash"), bytes("F"));
    l2.put_count = 0;

    StringTieredStore store(l2);
    std::string out;
    TEST_ASSERT_TRUE(store.get("flash", out));
    TEST_ASSERT_EQUAL(1u, store.size());

    for (int i = 0; i < USTORE_L1_CACHE_SIZE; i++) {
        char key[8];
        snprintf(key, sizeof(key), "n%d", i);
        TEST_ASSERT_TRUE(store.put(key, "v", 0, 0));
    }

    TEST_ASSERT_TRUE(store.exists("flash"));
    TEST_ASSERT_EQUAL((size_t)USTORE_L1_CACHE_SIZE + 1u, store.size());
}

void test_typed_tiered_store_overwrite_existing_l2_record() {
    CountingStore l2;
    l2.init();
    l2.put(bytes("a"), bytes("old"));
    l2.put_count = 0;

    StringTieredStore store(l2);
    TEST_ASSERT_TRUE(store.put("a", "new", 0, 0));
    TEST_ASSERT_EQUAL(1u, store.size());
    TEST_ASSERT_EQUAL_STRING("old", read_l2_string(l2, "a").c_str());

    TEST_ASSERT_TRUE(store.sync());
    TEST_ASSERT_EQUAL(1, l2.put_count);
    TEST_ASSERT_EQUAL_STRING("new", read_l2_string(l2, "a").c_str());
    TEST_ASSERT_EQUAL(1u, store.size());
}

void test_typed_tiered_store_sync_drops_dirty_entry_pruned_from_l2() {
    CountingStore l2;
    l2.init();
    l2.put(bytes("gone"), bytes("old"));
    l2.put_count = 0;

    StringTieredStore store(l2);
    std::string out;
    TEST_ASSERT_TRUE(store.get("gone", out));
    TEST_ASSERT_TRUE(store.put("gone", "new", 0, 0));
    TEST_ASSERT_EQUAL(1u, store.dirtyCount());

    TEST_ASSERT_TRUE(l2.remove(bytes("gone")));
    l2.put_count = 0;

    TEST_ASSERT_TRUE(store.sync());
    TEST_ASSERT_EQUAL(0, l2.put_count);
    TEST_ASSERT_FALSE(l2.exists(bytes("gone")));
    TEST_ASSERT_EQUAL(0u, store.dirtyCount());
    TEST_ASSERT_FALSE(store.exists("gone"));
}

void test_typed_tiered_store_remove_clears_l1_and_l2() {
    CountingStore l2;
    l2.init();
    StringTieredStore store(l2);

    store.put("a", "old", 0, 0);
    store.sync();
    store.put("a", "new", 0, 0);

    TEST_ASSERT_TRUE(store.remove("a"));
    TEST_ASSERT_FALSE(store.exists("a"));
    TEST_ASSERT_FALSE(l2.exists(bytes("a")));
    TEST_ASSERT_EQUAL(1, l2.remove_count);
    TEST_ASSERT_EQUAL(0u, store.size());
}

void test_typed_tiered_store_iterator_merges_l1_and_l2() {
    CountingStore l2;
    l2.init();
    l2.put(bytes("flash"), bytes("F"));
    l2.put_count = 0;

    StringTieredStore store(l2);
    store.put("ram", "R", 0, 0);
    store.put("flash", "updated", 0, 0);

    std::map<std::string, std::string> seen;
    for (auto& e : store) {
        seen[e.key] = e.value;
    }

    TEST_ASSERT_EQUAL(2, (int)seen.size());
    TEST_ASSERT_EQUAL_STRING("R", seen["ram"].c_str());
    TEST_ASSERT_EQUAL_STRING("updated", seen["flash"].c_str());
    TEST_ASSERT_EQUAL(0, l2.put_count);
}

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_typed_tiered_store_defers_put_until_sync);
    RUN_TEST(test_typed_tiered_store_evicts_lru_dirty_tail);
    RUN_TEST(test_typed_tiered_store_sync_flushes_all_dirty_entries);
    RUN_TEST(test_typed_tiered_store_flushes_remaining_ttl);
    RUN_TEST(test_typed_tiered_store_flushes_priority);
    RUN_TEST(test_typed_tiered_store_uses_l2_store_for_existing_keys);
    RUN_TEST(test_typed_tiered_store_promoted_l2_entry_is_not_double_counted);
    RUN_TEST(test_typed_tiered_store_clean_l2_entry_remains_visible_after_eviction);
    RUN_TEST(test_typed_tiered_store_overwrite_existing_l2_record);
    RUN_TEST(test_typed_tiered_store_sync_drops_dirty_entry_pruned_from_l2);
    RUN_TEST(test_typed_tiered_store_remove_clears_l1_and_l2);
    RUN_TEST(test_typed_tiered_store_iterator_merges_l1_and_l2);
    return UNITY_END();
}

int main(void) {
    return runUnityTests();
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    runUnityTests();
}
void loop() {}
#endif

void app_main() {
    runUnityTests();
}
