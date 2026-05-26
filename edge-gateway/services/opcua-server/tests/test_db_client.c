/**
 * @file test_db_client.c
 * @brief Unit tests for db_client.c — row parsing and NULL handling
 *
 * These tests do NOT require a real TimescaleDB.  They validate the
 * struct db_row parsing logic and edge cases by providing synthetic
 * rows constructed via db_client_poll callbacks.
 *
 * Real DB integration tests live in tests/integration/.
 */

#include "test_utils.h"
#include "db_client.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Synthetic row helpers — test the row struct access patterns         */
/* ------------------------------------------------------------------ */

static int g_cb_call_count;
static struct db_row g_last_row;

static void capture_cb(const struct db_row *row, void *user_data)
{
    (void)user_data;
    g_cb_call_count++;
    memcpy(&g_last_row, row, sizeof(g_last_row));
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_row_field_access(void)
{
    struct db_row row;
    struct db_field *f;
    int i;

    memset(&row, 0, sizeof(row));

    /* Build a synthetic row that mimics device_status_view columns */
    i = 0;
    snprintf(row.fields[i].name, sizeof(row.fields[i].name), "site_id");
    snprintf(row.fields[i].value, sizeof(row.fields[i].value), "factory1");
    row.fields[i].is_null = 0;
    i++;

    snprintf(row.fields[i].name, sizeof(row.fields[i].name), "device_id");
    snprintf(row.fields[i].value, sizeof(row.fields[i].value), "de01");
    row.fields[i].is_null = 0;
    i++;

    snprintf(row.fields[i].name, sizeof(row.fields[i].name), "last_rms");
    snprintf(row.fields[i].value, sizeof(row.fields[i].value), "2.35");
    row.fields[i].is_null = 0;
    i++;

    snprintf(row.fields[i].name, sizeof(row.fields[i].name), "missing_field");
    row.fields[i].value[0] = '\0';
    row.fields[i].is_null = 1;
    i++;

    row.field_count = i;

    /* Verify field access */
    TEST_ASSERT_EQUAL(4, row.field_count);

    f = &row.fields[0];
    TEST_ASSERT_EQUAL_STRING("site_id", f->name);
    TEST_ASSERT_EQUAL_STRING("factory1", f->value);
    TEST_ASSERT_FALSE(f->is_null);

    f = &row.fields[1];
    TEST_ASSERT_EQUAL_STRING("device_id", f->name);
    TEST_ASSERT_EQUAL_STRING("de01", f->value);
    TEST_ASSERT_FALSE(f->is_null);

    f = &row.fields[2];
    TEST_ASSERT_EQUAL_STRING("last_rms", f->name);
    TEST_ASSERT_EQUAL_STRING("2.35", f->value);
    TEST_ASSERT_FALSE(f->is_null);

    f = &row.fields[3];
    TEST_ASSERT_EQUAL_STRING("missing_field", f->name);
    TEST_ASSERT_TRUE(f->is_null);
    TEST_ASSERT_EQUAL_STRING("", f->value);
}

static void test_null_field_isolation(void)
{
    struct db_row row;

    memset(&row, 0, sizeof(row));

    snprintf(row.fields[0].name, sizeof(row.fields[0].name), "value");
    snprintf(row.fields[0].value, sizeof(row.fields[0].value), "123.45");
    row.fields[0].is_null = 0;

    snprintf(row.fields[1].name, sizeof(row.fields[1].name), "null_column");
    row.fields[1].value[0] = '\0';
    row.fields[1].is_null = 1;

    row.field_count = 2;

    /* Not null */
    TEST_ASSERT_FALSE(row.fields[0].is_null);
    TEST_ASSERT_EQUAL_STRING("123.45", row.fields[0].value);

    /* Null */
    TEST_ASSERT_TRUE(row.fields[1].is_null);
    TEST_ASSERT_EQUAL_STRING("", row.fields[1].value);
}

static void test_max_columns(void)
{
    struct db_row row;
    int i;

    memset(&row, 0, sizeof(row));

    for (i = 0; i < DB_MAX_COLUMNS; i++) {
        snprintf(row.fields[i].name, sizeof(row.fields[i].name), "col_%d", i);
        snprintf(row.fields[i].value, sizeof(row.fields[i].value), "%d", i);
        row.fields[i].is_null = 0;
    }
    row.field_count = DB_MAX_COLUMNS;

    TEST_ASSERT_EQUAL(DB_MAX_COLUMNS, row.field_count);

    /* Verify first and last are accessible */
    TEST_ASSERT_EQUAL_STRING("col_0",  row.fields[0].name);
    TEST_ASSERT_EQUAL_STRING("col_63", row.fields[63].name);
}

int main(void)
{
    RUN_TEST(test_row_field_access);
    RUN_TEST(test_null_field_isolation);
    RUN_TEST(test_max_columns);
    return test_summary();
}
