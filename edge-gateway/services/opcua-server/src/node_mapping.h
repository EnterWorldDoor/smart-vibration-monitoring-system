/**
 * @file node_mapping.h
 * @brief OPC UA NodeId <-> TimescaleDB column mapping table
 *
 * Static-array-based bidirectional map.  At startup the address-space builder
 * registers every leaf variable node; during the 1s poll loop the DB row
 * callback looks up the (device_key, column) pair to find the NodeId to update.
 */

#ifndef NODE_MAPPING_H
#define NODE_MAPPING_H

#include <open62541/server.h>

#define MAX_VARIABLES   256
#define MAX_DEVICE_KEY  128
#define MAX_COLUMN_NAME 64

/**
 * @brief One mapping entry: an OPC UA node bound to a DB column.
 */
struct node_mapping {
    UA_UInt32 node_id;                   /* OPC UA NodeId (ns=1, numeric) */
    char      column[MAX_COLUMN_NAME];   /* DB column name, e.g. "last_rms" */
    char      device_key[MAX_DEVICE_KEY];/* "site_id/device_type/device_id" */
};

/**
 * @brief Reset the mapping table to empty.
 */
void node_mapping_init(void);

/**
 * @brief Register a variable node.
 * @return 0 on success, -1 if table is full.
 */
int  node_mapping_register(UA_UInt32 node_id,
                           const char *column,
                           const char *device_key);

/**
 * @brief Look up column name by NodeId.
 * @return column string, or NULL if not found.
 */
const char *node_mapping_find_column(UA_UInt32 node_id);

/**
 * @brief Look up device key by NodeId.
 * @return device_key string, or NULL if not found.
 */
const char *node_mapping_find_device(UA_UInt32 node_id);

/**
 * @brief Reverse lookup: find NodeId for a (device_key, column) pair.
 * @return node_id on success, 0 if not found (0 is not a valid NodeId in our
 *         allocation scheme which starts at 1000).
 */
UA_UInt32 node_mapping_find_node(const char *device_key, const char *column);

/**
 * @brief Return the current number of registered mappings.
 */
int  node_mapping_count(void);

#endif /* NODE_MAPPING_H */
