/**
 * @file test_config.c
 * @brief Unit tests for config.c — YAML parsing & defaults
 */

#include "test_utils.h"
#include "config.h"

#include <stdio.h>

static void test_defaults(void)
{
    struct app_config cfg;

    config_set_defaults(&cfg);

    TEST_ASSERT_EQUAL_STRING("opc.tcp://0.0.0.0:4840", cfg.server.endpoint);
    TEST_ASSERT_EQUAL_STRING("EdgeVib OPC UA Server",   cfg.server.app_name);
    TEST_ASSERT_EQUAL_STRING("factory1",                cfg.server.site_id);
    TEST_ASSERT_EQUAL(5432,  cfg.db.port);
    TEST_ASSERT_EQUAL(1000,  cfg.db.poll_interval_ms);
    TEST_ASSERT_EQUAL(1,     cfg.security.anonymous);
}

static void test_load_valid(void)
{
    struct app_config cfg;
    const char *test_yaml =
        "server:\n"
        "  endpoint: \"opc.tcp://0.0.0.0:4841\"\n"
        "  site_id: \"test_site\"\n"
        "timescaledb:\n"
        "  host: \"192.168.1.100\"\n"
        "  port: 5433\n"
        "  dbname: \"test_db\"\n"
        "security:\n"
        "  anonymous: false\n";
    FILE *fh;

    fh = fopen("/tmp/test_opcua_config.yaml", "w");
    TEST_ASSERT_NOT_NULL(fh);
    fputs(test_yaml, fh);
    fclose(fh);

    config_set_defaults(&cfg);
    TEST_ASSERT_EQUAL(0, config_load("/tmp/test_opcua_config.yaml", &cfg));

    /* Overridden values */
    TEST_ASSERT_EQUAL_STRING("opc.tcp://0.0.0.0:4841", cfg.server.endpoint);
    TEST_ASSERT_EQUAL_STRING("test_site",                cfg.server.site_id);
    TEST_ASSERT_EQUAL_STRING("localhost",            cfg.db.host);
    TEST_ASSERT_EQUAL(5432, cfg.db.port);
    TEST_ASSERT_EQUAL_STRING("edgevib_ts", cfg.db.dbname);
    TEST_ASSERT_EQUAL(0, cfg.security.anonymous);

    /* Default values remain */
    TEST_ASSERT_EQUAL_STRING("edgevib", cfg.db.user);

    remove("/tmp/test_opcua_config.yaml");
}

static void test_load_missing_file(void)
{
    struct app_config cfg;

    config_set_defaults(&cfg);
    TEST_ASSERT_EQUAL(-1, config_load("/tmp/nonexistent_opcua.yaml", &cfg));
}

int main(void)
{
    RUN_TEST(test_defaults);
    RUN_TEST(test_load_valid);
    RUN_TEST(test_load_missing_file);
    return test_summary();
}
