/**
 * @file config.h
 * @brief OPC UA Server — YAML configuration loading
 *
 * Parses a 4-section YAML config (server / timescaledb / security / logging)
 * using libyaml event-driven parser. Mirrors the rs232-gateway parsing pattern.
 */

#ifndef CONFIG_H
#define CONFIG_H

struct server_config {
    char endpoint[256];
    char app_name[128];
    char app_uri[256];
    char site_id[64];
};

struct timescaledb_config {
    char host[256];
    int  port;
    char dbname[128];
    char user[128];
    char password[128];
    int  poll_interval_ms;
};

struct security_config {
    int anonymous;
};

struct logging_config {
    char level[32];
};

struct app_config {
    struct server_config      server;
    struct timescaledb_config db;
    struct security_config    security;
    struct logging_config     logging;
};

/**
 * @brief Fill cfg with hardcoded defaults so the server runs without a config file.
 */
void config_set_defaults(struct app_config *cfg);

/**
 * @brief Parse a YAML config file and merge values into cfg.
 * @param path  Path to the YAML file.
 * @param cfg   Pre-initialised config struct (call config_set_defaults first).
 * @return 0 on success, -1 on error.
 */
int config_load(const char *path, struct app_config *cfg);

#endif /* CONFIG_H */
