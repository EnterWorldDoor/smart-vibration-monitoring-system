/**
 * @file test_node_mapping.c
 * @brief Unit tests for node_mapping.c — mapping table operations
 */

#include "test_utils.h"
#include "node_mapping.h"

#include <string.h>

static void test_init_empty(void)
{
    node_mapping_init();
    TEST_ASSERT_EQUAL(0, node_mapping_count());
}

static void test_register_and_find(void)
{
    node_mapping_init();

    TEST_ASSERT_EQUAL(0, node_mapping_register(5001, "rms_x", "f1/motor/de01"));
    TEST_ASSERT_EQUAL(0, node_mapping_register(5002, "rms_y", "f1/motor/de01"));
    TEST_ASSERT_EQUAL(0, node_mapping_register(5003, "last_rms", "f1/motor/nde01"));

    TEST_ASSERT_EQUAL(3, node_mapping_count());

    /* Forward lookup */
    TEST_ASSERT_EQUAL_STRING("rms_x", node_mapping_find_column(5001));
    TEST_ASSERT_EQUAL_STRING("rms_y", node_mapping_find_column(5002));
    TEST_ASSERT_EQUAL_STRING("last_rms", node_mapping_find_column(5003));
    TEST_ASSERT_EQUAL_STRING("f1/motor/de01",  node_mapping_find_device(5001));
    TEST_ASSERT_EQUAL_STRING("f1/motor/nde01", node_mapping_find_device(5003));

    /* Reverse lookup */
    TEST_ASSERT_EQUAL(5001, (int)node_mapping_find_node("f1/motor/de01",  "rms_x"));
    TEST_ASSERT_EQUAL(5002, (int)node_mapping_find_node("f1/motor/de01",  "rms_y"));
    TEST_ASSERT_EQUAL(5003, (int)node_mapping_find_node("f1/motor/nde01", "last_rms"));

    /* Not found reverse */
    TEST_ASSERT_EQUAL(0, (int)node_mapping_find_node("f1/motor/de01", "nonexistent"));
    TEST_ASSERT_EQUAL(0, (int)node_mapping_find_node("nonexistent",   "rms_x"));
}

static void test_not_found(void)
{
    node_mapping_init();

    TEST_ASSERT_NULL(node_mapping_find_column(9999));
    TEST_ASSERT_NULL(node_mapping_find_device(9999));
    TEST_ASSERT_EQUAL(0, (int)node_mapping_find_node("a/b/c", "x"));
}

static void test_boundary(void)
{
    int i;

    node_mapping_init();

    /* Fill to MAX_VARIABLES */
    for (i = 0; i < MAX_VARIABLES; i++) {
        char col[32], dev[32];

        snprintf(col, sizeof(col), "col_%d", i);
        snprintf(dev, sizeof(dev), "dev_%d", i);
        TEST_ASSERT_EQUAL(0, node_mapping_register((UA_UInt32)(5000 + i), col, dev));
    }

    TEST_ASSERT_EQUAL(MAX_VARIABLES, node_mapping_count());

    /* One more should fail */
    TEST_ASSERT_EQUAL(-1, node_mapping_register(9999, "extra", "extra"));
    TEST_ASSERT_EQUAL(MAX_VARIABLES, node_mapping_count());

    /* Verify the first and last are still findable */
    TEST_ASSERT_EQUAL_STRING("col_0", node_mapping_find_column(5000));
    TEST_ASSERT_EQUAL_STRING("col_255", node_mapping_find_column(5255));
}

int main(void)
{
    RUN_TEST(test_init_empty);
    RUN_TEST(test_register_and_find);
    RUN_TEST(test_not_found);
    RUN_TEST(test_boundary);
    return test_summary();
}
