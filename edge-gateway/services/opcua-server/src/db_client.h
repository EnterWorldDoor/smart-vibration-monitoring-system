/**
 * @file db_client.h
 * @brief OPC UA Server — TimescaleDB client (libpq)
 *
 * Single persistent connection, single batch query per poll cycle.
 * Row-oriented callback API — the caller receives each row as a
 * struct db_row and maps columns to OPC UA node values.
 */

#ifndef DB_CLIENT_H
#define DB_CLIENT_H

#include "config.h"

#include <stddef.h>

#define DB_MAX_COLUMNS  64
#define DB_MAX_COLNAME  64
#define DB_MAX_COLVALUE 256

/**
 * @brief One column value from a result row.
 */
struct db_field {
    char name[DB_MAX_COLNAME];
    char value[DB_MAX_COLVALUE];
    int  is_null;
};

/**
 * @brief One row of query results.
 */
struct db_row {
    struct db_field fields[DB_MAX_COLUMNS];
    int             field_count;
};

/**
 * @brief Per-row callback during db_client_poll().
 * @param row       The current result row (valid only during the callback).
 * @param user_data Opaque pointer passed through from db_client_poll().
 */
typedef void (*db_row_callback_t)(const struct db_row *row, void *user_data);

/**
 * @brief Open a persistent connection to TimescaleDB.
 * @param cfg  Database connection parameters.
 * @return 0 on success, -1 on failure.
 */
int  db_client_init(const struct timescaledb_config *cfg);

/**
 * @brief Close the database connection.
 */
void db_client_deinit(void);

/**
 * @brief Execute a SQL query and invoke cb for each result row.
 *
 * Uses PQexec() — synchronous but latency is <5ms for device_status_view
 * in a 2-10 device scenario. The callback fires once per row.
 *
 * @param query      SQL statement.
 * @param cb         Called once per row.  NULL is allowed (rows are discarded).
 * @param user_data  Passed through to cb.
 * @return 0 on success, -1 on error.
 */
int  db_client_poll(const char *query, db_row_callback_t cb, void *user_data);

/**
 * @brief Check whether the persistent connection is healthy.
 * @return 1 if CONNECTION_OK, 0 otherwise.
 */
int  db_client_is_connected(void);

/**
 * @brief Attempt reconnection using PQreset().
 * @return 0 on success, -1 on failure.
 */
int  db_client_reconnect(void);

#endif /* DB_CLIENT_H */
