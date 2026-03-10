/*********************************************************************************
 * Test to reproduce the root collapse bug where Meta buffer gets stuck waiting
 * for a CLEAN new_root buffer during CP flush.
 *
 * Bug Description:
 * When GC removes the last entry from root, causing root to collapse (tree height
 * reduction), the new_root (which is the old child promoted to root) is CLEAN but
 * is incorrectly added as a dependency of Meta buffer. Since CLEAN buffers never
 * enter the flush queue, Meta's down_wait# never reaches 0, causing CP to hang.
 *
 *********************************************************************************/
#include <gtest/gtest.h>
#include <boost/uuid/random_generator.hpp>

#include <sisl/utility/enum.hpp>
#include "common/homestore_config.hpp"
#include "common/resource_mgr.hpp"
#include "test_common/homestore_test_common.hpp"
#include "btree_helpers/btree_test_helper.hpp"
#include "btree_helpers/btree_test_kvs.hpp"
#include "btree_helpers/btree_decls.h"

using namespace homestore;

SISL_LOGGING_INIT(HOMESTORE_LOG_MODS)
SISL_OPTIONS_ENABLE(logging, test_root_collapse_bug, iomgr, test_common_setup)
SISL_LOGGING_DECL(test_root_collapse_bug)

// Define options required by BtreeTestHelper
// NOTE: num_entries must be >= actual entries used in test (shadow_map size)
SISL_OPTION_GROUP(test_root_collapse_bug,
                  (num_entries, "", "num_entries", "number of entries for test",
                   ::cxxopts::value< uint32_t >()->default_value("2000"), "number"),
                  (num_iters, "", "num_iters", "number of iterations",
                   ::cxxopts::value< uint32_t >()->default_value("1"), "number"),
                  (run_time, "", "run_time", "run time",
                   ::cxxopts::value< uint32_t >()->default_value("30"), "seconds"),
                  (max_merge_level, "", "max_merge_level", "max merge level",
                   ::cxxopts::value< uint8_t >()->default_value("3"), "number"),
                  (preload_size, "", "preload_size", "preload size",
                   ::cxxopts::value< uint32_t >()->default_value("0"), "number"),
                  (disable_merge, "", "disable_merge", "disable merge",
                   ::cxxopts::value< bool >()->default_value("false"), ""))

class RootCollapseBugTest : public BtreeTestHelper< FixedLenBtree >, public ::testing::Test {
public:
    using T = FixedLenBtree;
    using K = typename T::KeyType;
    using V = typename T::ValueType;

    class TestIndexServiceCallbacks : public IndexServiceCallbacks {
    public:
        TestIndexServiceCallbacks(RootCollapseBugTest* test) : m_test(test) {}
        std::shared_ptr< IndexTableBase > on_index_table_found(superblk< index_table_sb >&& sb) override {
            LOGINFO("Index table recovered");
            m_test->m_bt = std::make_shared< typename T::BtreeType >(std::move(sb), m_test->m_cfg);
            return m_test->m_bt;
        }

    private:
        RootCollapseBugTest* m_test;
    };

    RootCollapseBugTest() : testing::Test() {}

    void SetUp() override {
        m_helper.start_homestore(
            "test_root_collapse_bug",
            {{HS_SERVICE::META, {.size_pct = 10.0}},
             {HS_SERVICE::INDEX, {.size_pct = 70.0, .index_svc_cbs = new TestIndexServiceCallbacks(this)}}});

        LOGINFO("Node size {}", hs()->index_service().node_size());
        this->m_cfg = BtreeConfig(hs()->index_service().node_size());

        auto uuid = boost::uuids::random_generator()();
        auto parent_uuid = boost::uuids::random_generator()();

        // Configure cache settings like test_index_btree
        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.generic.cache_max_throttle_cnt = 10000;
            HS_SETTINGS_FACTORY().save();
        });
        homestore::hs()->resource_mgr().reset_dirty_buf_qd();

        // Create index table and attach to index service
        BtreeTestHelper< T >::SetUp();
        this->m_bt = std::make_shared< typename T::BtreeType >(uuid, parent_uuid, 0, this->m_cfg);
        hs()->index_service().add_index_table(this->m_bt);
        LOGINFO("Added index table to index service");
    }

    void TearDown() override {
        BtreeTestHelper< T >::TearDown();
        m_helper.shutdown_homestore(false);
        this->m_bt.reset();
    }

    // Access m_sb_buffer through public method or friend declaration
    // For now, we'll use a simpler approach: just trigger CP and see if it hangs
    // The hang itself is proof of the bug

    test_common::HSTestHelper m_helper;
};

/**
 * Test Strategy:
 * 1. Insert enough entries to create a 2-level tree (root INTERIOR + child LEAF)
 * 2. Remove all entries to make root empty (only edge to child remains)
 * 3. This triggers check_collapse_root() which calls on_root_changed(child)
 *    - BUG: link_buf(Meta, child, false) adds CLEAN child as dependency
 * 4. Trigger CP flush and verify it completes (or hangs if bug exists)
 *
 * Expected behavior WITH bug:
 *   - CP will timeout because Meta waits for CLEAN new_root to flush
 *   - Test will FAIL with timeout error
 *
 * Expected behavior AFTER fix:
 *   - CP completes successfully
 *   - Test PASSES
 */
TEST_F(RootCollapseBugTest, ReproduceMetaWaitingForCleanRoot) {
    LOGINFO("=== Starting Root Collapse Bug Reproduction ===");

    // Step 1: Insert entries to create a 2-level tree
    // We need enough entries to split root into INTERIOR + LEAF
    // Based on test_index_btree: 7000 entries → interior=1, leaf=41
    // Node capacity is ~170+ entries, so use 1000 to be safe
    const uint32_t num_entries = 1000;
    LOGINFO("Step 1: Inserting {} entries to create 2-level tree", num_entries);

    for (uint32_t i = 0; i < num_entries; ++i) {
        this->put(i, btree_put_type::INSERT);
    }

    // Verify tree structure
    auto [interior_count, leaf_count] = this->m_bt->compute_node_count();
    LOGINFO("After insert: interior_count={}, leaf_count={}, tree_depth={}",
            interior_count, leaf_count, this->m_bt->get_btree_depth());

    // We need at least 1 interior and 1 leaf (2-level tree)
    ASSERT_GE(interior_count, 1) << "Tree should have at least one interior node";
    ASSERT_GE(leaf_count, 1) << "Tree should have at least one leaf node";

    uint32_t initial_depth = this->m_bt->get_btree_depth();

    // Step 2: Remove all entries to make root empty
    LOGINFO("Step 2: Removing all {} entries to trigger root collapse", num_entries);

    for (uint32_t i = 0; i < num_entries; ++i) {
        this->remove_one(i);
    }

    // Verify tree structure after removal
    auto [interior_after, leaf_after] = this->m_bt->compute_node_count();
    LOGINFO("After removal: interior_count={}, leaf_count={}, tree_depth={}",
            interior_after, leaf_after, this->m_bt->get_btree_depth());

    uint32_t final_depth = this->m_bt->get_btree_depth();

    // After root collapse, tree depth should have decreased
    if (final_depth < initial_depth) {
        LOGINFO("Root collapse detected: depth decreased from {} to {}", initial_depth, final_depth);
    } else {
        LOGWARN("Root did not collapse (depth unchanged). Bug may not reproduce.");
    }

    // Step 3: Trigger CP and verify it completes (or hangs)
    LOGINFO("Step 3: Triggering CP flush...");

    // Set a timeout to detect if CP hangs
    std::atomic< bool > cp_completed{false};
    std::thread cp_thread([this, &cp_completed]() {
        try {
            test_common::HSTestHelper::trigger_cp(true /* wait */);
            cp_completed.store(true);
            LOGINFO("CP flush completed successfully");
        } catch (const std::exception& e) {
            LOGERROR("CP flush failed with exception: {}", e.what());
        }
    });

    // Wait for CP with timeout (10 seconds should be enough for normal CP)
    bool timeout = false;
    for (int i = 0; i < 100; ++i) {  // 100 * 100ms = 10 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cp_completed.load()) {
            break;
        }
        if (i == 99) {
            timeout = true;
        }
    }

    if (timeout) {
        LOGERROR("========================================");
        LOGERROR("CP TIMEOUT DETECTED!");
        LOGERROR("CP did not complete within 10 seconds");
        LOGERROR("This confirms the bug:");
        LOGERROR("  - Meta buffer is waiting for CLEAN new_root");
        LOGERROR("  - CLEAN buffers never enter flush queue");
        LOGERROR("  - Meta's down_wait# never reaches 0");
        LOGERROR("========================================");

        // Try to join thread (it will continue hanging)
        cp_thread.detach();  // Detach to avoid blocking test exit

        // This test is expected to fail until the bug is fixed
        FAIL() << "CP timeout detected - Meta buffer stuck waiting for CLEAN new_root";
    } else {
        LOGINFO("CP completed successfully");
        cp_thread.join();
        LOGINFO("Test PASSED - Bug is fixed or did not reproduce");
    }

    LOGINFO("=== Root Collapse Bug Test Complete ===");
}

/**
 * Alternative test: Use a smaller tree to make root collapse more predictable
 * This test uses fewer entries to ensure root collapse happens reliably
 */
TEST_F(RootCollapseBugTest, SmallTreeRootCollapse) {
    LOGINFO("=== Small Tree Root Collapse Test ===");

    /* Strategy to trigger root collapse:
     * 1. Insert just enough to split root into INTERIOR + 2 children
     * 2. Delete ONLY left child's entries (remove half of the data)
     * 3. Left child becomes empty, merge removes it
     * 4. Root left with only 1 edge, total_entries() == 0
     * 5. Triggers check_collapse_root()!
     *
     * Tree structure:
     *   Root INTERIOR [split_key]
     *        /              \
     *   Left child      Right child
     *   (0..split-1)    (split..N)
     *
     * After deleting left half:
     *   Root INTERIOR []  (no entries!)
     *        \
     *      Right child
     *      (split..N)
     */

    // Node capacity is ~170-200 based on empirical testing
    // Use 600 to create 3 children (each ~200 entries)
    const uint32_t total_entries = 600;

    // Delete enough to merge first 2 children completely,
    // leaving root with only the 3rd child as edge
    // Try 300: should clear first 2 children (~400 entries / 2 = ~200 each)
    const uint32_t delete_count = 300;

    LOGINFO("Step 1: Insert {} entries to force split", total_entries);
    for (uint32_t i = 0; i < total_entries; ++i) {
        this->put(i, btree_put_type::INSERT);
    }

    auto [interior_before, leaf_before] = this->m_bt->compute_node_count();
    uint32_t depth_before = this->m_bt->get_btree_depth();
    LOGINFO("After insert: depth={}, interior={}, leaf={}", depth_before, interior_before, leaf_before);

    if (depth_before == 0) {
        LOGWARN("Tree did not split - need more entries. Skipping test.");
        return;
    }

    // Step 2: Delete partial entries to trigger merge → root becomes only edge
    LOGINFO("Step 2: Deleting {} entries to trigger merge (not collapse)", delete_count);
    for (uint32_t i = 0; i < delete_count; ++i) {
        this->remove_one(i);
    }

    auto [interior_mid, leaf_mid] = this->m_bt->compute_node_count();
    uint32_t depth_mid = this->m_bt->get_btree_depth();
    LOGINFO("After merge: depth={}, interior={}, leaf={}", depth_mid, interior_mid, leaf_mid);

    // CRITICAL: Trigger CP to flush all changes
    // After this CP, the edge child will be CLEAN in the next CP
    LOGINFO("Step 2.5: Triggering CP to flush merge changes");
    test_common::HSTestHelper::trigger_cp(true /* wait */);
    LOGINFO("CP completed - all buffers from Step 1-2 are now CLEAN");

    // Step 3: Remove non-existent key in NEW CP to trigger collapse with CLEAN child
    // NOW the edge child hasn't been modified in this NEW CP, so it's CLEAN!
    // Key insight: The edge child (merged node) hasn't been modified in this operation,
    // so it might be CLEAN. When it's promoted to new_root:
    // 1. check_collapse_root() calls on_root_changed(CLEAN_child)
    // 2. link_buf(Meta, CLEAN_child) adds dependency
    // 3. Retry finds key doesn't exist → returns not_found
    // 4. No write_node(new_root) happens!
    // 5. New_root stays CLEAN → CP will hang waiting for it
    LOGINFO("Step 3: Removing non-existent key to trigger collapse");
    const uint32_t non_existent_key = total_entries + 1000;
    LOGINFO("Attempting to remove key {} (doesn't exist)", non_existent_key);
    this->remove_one(non_existent_key);  // Will trigger collapse on retry

    auto [interior_after, leaf_after] = this->m_bt->compute_node_count();
    uint32_t depth_after = this->m_bt->get_btree_depth();
    uint64_t keys_after = this->m_bt->count_keys(this->m_bt->root_node_id());

    LOGINFO("After collapse attempt: depth={}, interior={}, leaf={}, keys={}",
            depth_after, interior_after, leaf_after, keys_after);

    if (depth_after < depth_mid) {
        LOGINFO("✓✓✓ ROOT COLLAPSE DETECTED via non-existent key! ✓✓✓");
        LOGINFO("Tree height decreased from {} to {}", depth_mid, depth_after);
        LOGINFO("Edge child promoted to new_root - checking if it's CLEAN...");
    } else {
        LOGWARN("Root collapse did not occur - depth unchanged");
    }

    // Try CP with timeout
    LOGINFO("Triggering CP...");

    std::atomic< bool > cp_done{false};
    std::thread t([this, &cp_done]() {
        test_common::HSTestHelper::trigger_cp(true /* wait */);
        cp_done.store(true);
    });

    // Wait 10 seconds max
    for (int i = 0; i < 100 && !cp_done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!cp_done.load()) {
        LOGERROR("CP timeout in SmallTreeRootCollapse test");
        t.detach();
        FAIL() << "CP timeout - bug reproduced";
    } else {
        t.join();
        LOGINFO("CP completed successfully");
    }
}

int main(int argc, char* argv[]) {
    int parsed_argc{argc};
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging, test_root_collapse_bug, iomgr, test_common_setup);
    sisl::logging::SetLogger("test_root_collapse_bug");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    return RUN_ALL_TESTS();
}
