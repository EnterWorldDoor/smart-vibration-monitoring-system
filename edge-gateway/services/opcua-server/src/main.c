/**
 * @file main.c
 * @brief OPC UA Server — entry point, signal handling, 1s polling loop
 *
 * Architecture mirrors rs232-gateway main.c:
 *   1. Load config (defaults + YAML)
 *   2. Setup signal handlers (SIGTERM/SIGINT → graceful shutdown)
 *   3. DB connect + device discovery + OPC UA address space construction
 *   4. Main loop: DB poll → update nodes → iterate server → sleep(1s)
 *   5. Shutdown: deinit server → deinit DB → exit
 */

#include "config.h"
#include "db_client.h"
#include "node_mapping.h"
#include "opcua_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Logging macros (mirror rs232-gateway prefix pattern)                */
/* ------------------------------------------------------------------ */

#define log_info(fmt, ...) \
    fprintf(stdout, "[%ld] INFO  opcua-srv: " fmt "\n", \
            (long)time(NULL), ##__VA_ARGS__)

#define log_warn(fmt, ...) \
    fprintf(stderr, "[%ld] WARN  opcua-srv: " fmt "\n", \
            (long)time(NULL), ##__VA_ARGS__)

#define log_error(fmt, ...) \
    fprintf(stderr, "[%ld] ERROR opcua-srv: " fmt "\n", \
            (long)time(NULL), ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* DB poll callback — invoked once per row from device_status_view     */
/* ------------------------------------------------------------------ */

static void on_db_row(const struct db_row *row, void *user_data)
{
    (void)user_data;
    opcua_server_update_values(row);
}

/* ------------------------------------------------------------------ */
/* Primary DB query — LEFT JOIN device_status_view with vibration_view */
/* ------------------------------------------------------------------ */

#define DEVICE_QUERY \
    "SELECT " \
    "  d.last_seen, d.site_id, d.device_type, d.device_id, " \
    "  d.service_state, d.data_quality, d.last_rms, d.last_temperature, " \
    "  d.last_ai_class, d.last_ai_confidence, " \
    "  v.rms_x, v.rms_y, v.rms_z, v.overall_rms, " \
    "  v.peak_frequency_hz, v.peak_amplitude_g " \
    "FROM device_status_view d " \
    "LEFT JOIN vibration_view v " \
    "  ON  d.site_id     = v.site_id " \
    "  AND d.device_type = v.device_type " \
    "  AND d.device_id   = v.device_id " \
    "  AND d.last_seen   = v.time"

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char          *config_path;
    struct app_config    cfg;
    struct sigaction     sa;
    int                  db_ok;

    /* 1. Config path */
    config_path = (argc > 1) ? argv[1] : "config/opcua-server.yaml";

    /* 2. Load config */
    config_set_defaults(&cfg);
    if (config_load(config_path, &cfg) != 0) {
        log_error("failed to load config from '%s'", config_path);
        return EXIT_FAILURE;
    }

    log_info("starting — endpoint %s", cfg.server.endpoint);
    log_info("site '%s', DB %s:%d/%s, poll %dms",
             cfg.server.site_id,
             cfg.db.host, cfg.db.port, cfg.db.dbname,
             cfg.db.poll_interval_ms);

    /* 3. Signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* 4. DB connect */
    if (db_client_init(&cfg.db) != 0) {
        log_error("cannot connect to TimescaleDB — exiting");
        return EXIT_FAILURE;
    }
    log_info("connected to TimescaleDB");

    /* 5. OPC UA server initialisation (includes device discovery) */
    if (opcua_server_init(&cfg.server, &cfg.security, &cfg.db) != 0) {
        log_error("opcua_server_init failed — exiting");
        db_client_deinit();
        return EXIT_FAILURE;
    }

    /* 6. Main polling loop */
    log_info("entering main poll loop (interval %dms)", cfg.db.poll_interval_ms);

    while (g_running) {
        /* Reconnect DB if needed */
        if (!db_client_is_connected()) {
            log_warn("DB disconnected, attempting reconnect...");
            if (db_client_reconnect() == 0) {
                log_info("DB reconnected");
            } else {
                opcua_server_mark_all_bad();
                sleep(2);
                continue;
            }
        }

        /* Poll latest data */
        db_ok = db_client_poll(DEVICE_QUERY, on_db_row, NULL);
        if (db_ok != 0) {
            log_warn("DB poll failed");
            opcua_server_mark_all_bad();
            sleep(1);
            continue;
        }

        /* Process OPC UA network events (non-blocking) */
        opcua_server_iterate();

        /* Sleep for the poll interval */
        usleep((useconds_t)cfg.db.poll_interval_ms * 1000);
    }

    /* 7. Shutdown */
    log_info("shutting down...");
    opcua_server_deinit();
    db_client_deinit();
    log_info("stopped");

    return EXIT_SUCCESS;
}
