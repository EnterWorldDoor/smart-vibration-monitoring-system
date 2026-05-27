/**
 * @file node_mapping.c
 * @brief Static-array mapping table implementation
 *
 * Simple append-only table with linear scan lookups.  MAX_VARIABLES=256
 * means even a full scan is <1 us — no hash table needed.
 */

#include "node_mapping.h"

#include <stdio.h>
#include <string.h>

static struct node_mapping g_mappings[MAX_VARIABLES];
static int                 g_count;

void node_mapping_init(void)
{
    memset(g_mappings, 0, sizeof(g_mappings));
    g_count = 0;
}

int node_mapping_register(UA_UInt32 node_id,
                          const char *column,
                          const char *device_key)
{
    struct node_mapping *m;

    if (g_count >= MAX_VARIABLES)
        return -1;

    m = &g_mappings[g_count++];
    m->node_id = node_id;

    if (column)
        snprintf(m->column, sizeof(m->column), "%s", column);
    if (device_key)
        snprintf(m->device_key, sizeof(m->device_key), "%s", device_key);

    return 0;
}

const char *node_mapping_find_column(UA_UInt32 node_id)
{
    int i;

    for (i = 0; i < g_count; i++) {
        if (g_mappings[i].node_id == node_id)
            return g_mappings[i].column;
    }
    return NULL;
}

const char *node_mapping_find_device(UA_UInt32 node_id)
{
    int i;

    for (i = 0; i < g_count; i++) {
        if (g_mappings[i].node_id == node_id)
            return g_mappings[i].device_key;
    }
    return NULL;
}

UA_UInt32 node_mapping_find_node(const char *device_key, const char *column)
{
    int i;

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_mappings[i].device_key, device_key) == 0 &&
            strcmp(g_mappings[i].column,     column) == 0)
            return g_mappings[i].node_id;
    }
    return 0;   /* 0 = not found (valid NodeIds start at 1000) */
}

int node_mapping_count(void)
{
    return g_count;
}
