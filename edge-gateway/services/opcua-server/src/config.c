/**
 * @file config.c
 * @brief libyaml event-driven YAML config parser
 *
 * Mirrors the parsing pattern from rs232-gateway main.c lines 100-218.
 * Uses a simple state machine over YAML tokens to fill struct app_config.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

/* Parser state machine */
enum parse_state {
    S_TOP,
    S_SECTION,
    S_KEY,
    S_VALUE
};

void config_set_defaults(struct app_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* server */
    snprintf(cfg->server.endpoint, sizeof(cfg->server.endpoint), "opc.tcp://0.0.0.0:4840");
    snprintf(cfg->server.app_name, sizeof(cfg->server.app_name), "EdgeVib OPC UA Server");
    snprintf(cfg->server.app_uri,  sizeof(cfg->server.app_uri),  "urn:edgevib:opcua-server");
    snprintf(cfg->server.site_id,  sizeof(cfg->server.site_id),  "factory1");

    /* timescaledb */
    snprintf(cfg->db.host,     sizeof(cfg->db.host),     "localhost");
    cfg->db.port = 5432;
    snprintf(cfg->db.dbname,   sizeof(cfg->db.dbname),   "edgevib_ts");
    snprintf(cfg->db.user,     sizeof(cfg->db.user),     "edgevib");
    snprintf(cfg->db.password, sizeof(cfg->db.password), "edgevib123");
    cfg->db.poll_interval_ms = 1000;

    /* security */
    cfg->security.anonymous = 1;

    /* logging */
    snprintf(cfg->logging.level, sizeof(cfg->logging.level), "info");
}

int config_load(const char *path, struct app_config *cfg)
{
    FILE *fh;
    yaml_parser_t parser;
    yaml_token_t  token;
    char current_section[64];
    char current_key[64];
    int  state;
    int  done;
    int  ret;

    fh = fopen(path, "r");
    if (!fh) {
        fprintf(stderr, "config: cannot open '%s'\n", path);
        return -1;
    }

    ret = -1;
    if (!yaml_parser_initialize(&parser)) {
        fprintf(stderr, "config: yaml_parser_initialize failed\n");
        goto close_file;
    }
    yaml_parser_set_input_file(&parser, fh);

    memset(current_section, 0, sizeof(current_section));
    memset(current_key, 0, sizeof(current_key));
    state = S_TOP;
    done = 0;

    while (!done) {
        if (!yaml_parser_scan(&parser, &token)) {
            fprintf(stderr, "config: YAML parse error at line %zu\n", parser.problem_mark.line + 1);
            goto clean_parser;
        }

        switch (token.type) {
        case YAML_STREAM_START_TOKEN:
        case YAML_STREAM_END_TOKEN:
        case YAML_DOCUMENT_START_TOKEN:
        case YAML_DOCUMENT_END_TOKEN:
            break;

        case YAML_BLOCK_MAPPING_START_TOKEN:
            if (state == S_TOP)
                state = S_SECTION;
            else
                state = S_KEY;
            break;

        case YAML_BLOCK_END_TOKEN:
            if (state == S_KEY || state == S_VALUE)
                state = S_SECTION;
            break;

        case YAML_KEY_TOKEN:
            state = S_KEY;
            break;

        case YAML_VALUE_TOKEN:
            state = S_VALUE;
            break;

        case YAML_SCALAR_TOKEN: {
            const char *val = (const char *)token.data.scalar.value;

            switch (state) {
            case S_SECTION:
            case S_KEY:
                if (current_section[0] == '\0' || state == S_SECTION) {
                    snprintf(current_section, sizeof(current_section), "%s", val);
                } else {
                    snprintf(current_key, sizeof(current_key), "%s", val);
                }
                break;

            case S_VALUE: {
                /* Fill the appropriate config field */
                if (strcmp(current_section, "server") == 0) {
                    if (strcmp(current_key, "endpoint") == 0)
                        snprintf(cfg->server.endpoint, sizeof(cfg->server.endpoint), "%s", val);
                    else if (strcmp(current_key, "app_name") == 0)
                        snprintf(cfg->server.app_name, sizeof(cfg->server.app_name), "%s", val);
                    else if (strcmp(current_key, "app_uri") == 0)
                        snprintf(cfg->server.app_uri, sizeof(cfg->server.app_uri), "%s", val);
                    else if (strcmp(current_key, "site_id") == 0)
                        snprintf(cfg->server.site_id, sizeof(cfg->server.site_id), "%s", val);
                } else if (strcmp(current_section, "timescaledb") == 0) {
                    if (strcmp(current_key, "host") == 0)
                        snprintf(cfg->db.host, sizeof(cfg->db.host), "%s", val);
                    else if (strcmp(current_key, "port") == 0)
                        cfg->db.port = atoi(val);
                    else if (strcmp(current_key, "dbname") == 0)
                        snprintf(cfg->db.dbname, sizeof(cfg->db.dbname), "%s", val);
                    else if (strcmp(current_key, "user") == 0)
                        snprintf(cfg->db.user, sizeof(cfg->db.user), "%s", val);
                    else if (strcmp(current_key, "password") == 0)
                        snprintf(cfg->db.password, sizeof(cfg->db.password), "%s", val);
                    else if (strcmp(current_key, "poll_interval_ms") == 0)
                        cfg->db.poll_interval_ms = atoi(val);
                } else if (strcmp(current_section, "security") == 0) {
                    if (strcmp(current_key, "anonymous") == 0)
                        cfg->security.anonymous = (strcmp(val, "true") == 0) ? 1 : atoi(val);
                } else if (strcmp(current_section, "logging") == 0) {
                    if (strcmp(current_key, "level") == 0)
                        snprintf(cfg->logging.level, sizeof(cfg->logging.level), "%s", val);
                }
                current_key[0] = '\0';
                break;
            }
            default:
                break;
            }
            break;
        }

        default:
            break;
        }

        if (token.type == YAML_STREAM_END_TOKEN)
            done = 1;

        yaml_token_delete(&token);
    }

    ret = 0;

clean_parser:
    yaml_parser_delete(&parser);
close_file:
    fclose(fh);
    return ret;
}
