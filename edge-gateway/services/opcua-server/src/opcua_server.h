/**
 * @file opcua_server.h
 * @brief OPC UA Server — open62541 wrapper
 *
 * Manages the UA_Server lifecycle: initialisation, dynamic address-space
 * construction, per-cycle node-value updates, and shutdown.
 */

#ifndef OPCUA_SERVER_H
#define OPCUA_SERVER_H

#include "config.h"
#include "db_client.h"

#include <open62541/server.h>

/**
 * @brief Initialise the OPC UA server and build the static address space.
 *
 * Creates a minimal-config server on the configured port, then queries
 * TimescaleDB once to discover devices and build the node tree.
 *
 * @param server_cfg  Server endpoint / site_id / app metadata.
 * @param sec_cfg     Security policy (anonymous vs user/pass).
 * @param db_cfg      DB connection parameters for device discovery.
 * @return 0 on success, -1 on failure.
 */
int opcua_server_init(const struct server_config  *server_cfg,
                      const struct security_config *sec_cfg,
                      const struct timescaledb_config *db_cfg);

/**
 * @brief Run one non-blocking iteration of the server event loop.
 *
 * Processes incoming OPC UA client requests (reads, subscriptions).
 * Returns immediately — call from the main 1s polling loop.
 *
 * @return Suggested wait interval in microseconds (can be ignored).
 */
UA_UInt32 opcua_server_iterate(void);

/**
 * @brief Update variable node values from a DB result row.
 *
 * Called once per row after each db_client_poll() cycle.  Looks up the
 * NodeId in the node_mapping table for each (device_key, column) pair
 * and calls UA_Server_writeValue().
 *
 * @param row  One row from the device_status_view JOIN query.
 */
void opcua_server_update_values(const struct db_row *row);

/**
 * @brief Mark all registered variable nodes as Bad.
 *
 * Called when the DB connection is lost or a query fails, so OPC UA
 * clients see the degraded state immediately.
 */
void opcua_server_mark_all_bad(void);

/**
 * @brief Shut down and free the server.
 */
void opcua_server_deinit(void);

#endif /* OPCUA_SERVER_H */
