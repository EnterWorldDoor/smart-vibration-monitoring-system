/**
 * @file opcua_server.c
 * @brief open62541 v1.4 server lifecycle & address-space management
 */

#include "opcua_server.h"
#include "node_mapping.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Static state                                                        */
/* ------------------------------------------------------------------ */

static UA_Server *g_server     = NULL;
static UA_UInt32  g_next_node  = 1000;

static UA_NodeId g_folder_edgevib;
static UA_NodeId g_folder_root;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static UA_NodeId find_or_create_folder(UA_NodeId parent,
                                       const char *browse_name_str)
{
    UA_BrowseDescription bd;
    UA_BrowseResult      br;
    size_t               i;

    UA_BrowseDescription_init(&bd);
    bd.nodeId         = parent;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    bd.includeSubtypes = UA_TRUE;
    bd.resultMask      = UA_BROWSERESULTMASK_ALL;

    br = UA_Server_browse(g_server, 100, &bd);
    for (i = 0; i < br.referencesSize; i++) {
        UA_ReferenceDescription *ref = &br.references[i];
        if (ref->nodeId.nodeId.identifierType == UA_NODEIDTYPE_NUMERIC) {
            size_t len;
            char  *s = (char *)ref->browseName.name.data;
            len = ref->browseName.name.length;
            if (len == strlen(browse_name_str) &&
                memcmp(s, browse_name_str, len) == 0) {
                UA_NodeId ret = ref->nodeId.nodeId;
                UA_BrowseResult_clear(&br);
                return ret;
            }
        }
    }
    UA_BrowseResult_clear(&br);

    /* Not found — create */
    {
        UA_NodeId          new_id;
        UA_QualifiedName   qn;
        UA_ObjectAttributes oa;

        new_id = UA_NODEID_NUMERIC(1, g_next_node++);
        qn     = UA_QUALIFIEDNAME(1, (char *)browse_name_str);
        oa     = UA_ObjectAttributes_default;

        oa.displayName = UA_LOCALIZEDTEXT((char *)"", (char *)browse_name_str);

        UA_Server_addObjectNode(g_server, new_id, parent,
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            qn, UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
            oa, NULL, NULL);
        return new_id;
    }
}

static UA_NodeId add_double_variable(UA_NodeId parent,
                                     const char *name)
{
    UA_NodeId            vid;
    UA_VariableAttributes va;
    UA_QualifiedName     qn;
    UA_Double            zero = 0.0;

    vid = UA_NODEID_NUMERIC(1, g_next_node++);
    qn  = UA_QUALIFIEDNAME(1, (char *)name);

    va = UA_VariableAttributes_default;
    va.displayName     = UA_LOCALIZEDTEXT((char *)"", (char *)name);
    va.accessLevel     = UA_ACCESSLEVELMASK_READ;
    va.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    va.dataType        = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    va.valueRank       = UA_VALUERANK_SCALAR;
    UA_Variant_setScalar(&va.value, &zero, &UA_TYPES[UA_TYPES_DOUBLE]);

    UA_Server_addVariableNode(g_server, vid, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ANALOGITEMTYPE), va, NULL, NULL);

    return vid;
}

static UA_NodeId add_string_variable(UA_NodeId parent, const char *name)
{
    UA_NodeId            vid;
    UA_VariableAttributes va;
    UA_QualifiedName     qn;
    UA_String            empty = UA_STRING((char *)"");

    vid = UA_NODEID_NUMERIC(1, g_next_node++);
    qn  = UA_QUALIFIEDNAME(1, (char *)name);

    va = UA_VariableAttributes_default;
    va.displayName     = UA_LOCALIZEDTEXT((char *)"", (char *)name);
    va.accessLevel     = UA_ACCESSLEVELMASK_READ;
    va.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    va.dataType        = UA_TYPES[UA_TYPES_STRING].typeId;
    va.valueRank       = UA_VALUERANK_SCALAR;
    UA_Variant_setScalar(&va.value, &empty, &UA_TYPES[UA_TYPES_STRING]);

    UA_Server_addVariableNode(g_server, vid, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn,
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), va, NULL, NULL);

    return vid;
}

static UA_NodeId add_int32_variable(UA_NodeId parent, const char *name)
{
    UA_NodeId            vid;
    UA_VariableAttributes va;
    UA_QualifiedName     qn;
    UA_Int32             zero = 0;

    vid = UA_NODEID_NUMERIC(1, g_next_node++);
    qn  = UA_QUALIFIEDNAME(1, (char *)name);

    va = UA_VariableAttributes_default;
    va.displayName     = UA_LOCALIZEDTEXT((char *)"", (char *)name);
    va.accessLevel     = UA_ACCESSLEVELMASK_READ;
    va.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    va.dataType        = UA_TYPES[UA_TYPES_INT32].typeId;
    va.valueRank       = UA_VALUERANK_SCALAR;
    UA_Variant_setScalar(&va.value, &zero, &UA_TYPES[UA_TYPES_INT32]);

    UA_Server_addVariableNode(g_server, vid, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn,
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), va, NULL, NULL);

    return vid;
}

/* ------------------------------------------------------------------ */
/* Device subtree construction                                          */
/* ------------------------------------------------------------------ */

static void opcua_server_add_device(const char *site_id,
                                    const char *device_type,
                                    const char *device_id)
{
    char      device_key[MAX_DEVICE_KEY];
    UA_NodeId folder_site, folder_type, folder_dev, folder, vid;

    snprintf(device_key, sizeof(device_key), "%s/%s/%s",
             site_id, device_type, device_id);

    folder_site = find_or_create_folder(g_folder_edgevib, site_id);
    folder_type = find_or_create_folder(folder_site, device_type);
    folder_dev  = find_or_create_folder(folder_type, device_id);

    /* ---- Vibration ---- */
    folder = find_or_create_folder(folder_dev, "Vibration");

    vid = add_double_variable(folder, "RMS_X");
    node_mapping_register(vid.identifier.numeric, "rms_x", device_key);
    vid = add_double_variable(folder, "RMS_Y");
    node_mapping_register(vid.identifier.numeric, "rms_y", device_key);
    vid = add_double_variable(folder, "RMS_Z");
    node_mapping_register(vid.identifier.numeric, "rms_z", device_key);
    vid = add_double_variable(folder, "Overall_RMS");
    node_mapping_register(vid.identifier.numeric, "overall_rms", device_key);
    vid = add_double_variable(folder, "PeakFrequency");
    node_mapping_register(vid.identifier.numeric, "peak_frequency_hz", device_key);
    vid = add_double_variable(folder, "PeakAmplitude");
    node_mapping_register(vid.identifier.numeric, "peak_amplitude_g", device_key);

    /* ---- AI_Diagnosis ---- */
    folder = find_or_create_folder(folder_dev, "AI_Diagnosis");

    vid = add_string_variable(folder, "ClassName");
    node_mapping_register(vid.identifier.numeric, "last_ai_class", device_key);
    vid = add_double_variable(folder, "Confidence");
    node_mapping_register(vid.identifier.numeric, "last_ai_confidence", device_key);

    /* ---- MotorData (motor devices only) ---- */
    if (strcmp(device_type, "motor") == 0) {
        folder = find_or_create_folder(folder_dev, "MotorData");
        vid = add_double_variable(folder, "Voltage");
        node_mapping_register(vid.identifier.numeric, "voltage", device_key);
        vid = add_double_variable(folder, "Current");
        node_mapping_register(vid.identifier.numeric, "current", device_key);
        vid = add_double_variable(folder, "Power");
        node_mapping_register(vid.identifier.numeric, "power", device_key);
    }

    /* ---- Status ---- */
    folder = find_or_create_folder(folder_dev, "Status");

    vid = add_string_variable(folder, "ServiceState");
    node_mapping_register(vid.identifier.numeric, "service_state", device_key);
    vid = add_int32_variable(folder, "DataQuality");
    node_mapping_register(vid.identifier.numeric, "data_quality", device_key);
    vid = add_double_variable(folder, "LastTemperature");
    node_mapping_register(vid.identifier.numeric, "last_temperature", device_key);
}

/* ------------------------------------------------------------------ */
/* Discovery callback                                                   */
/* ------------------------------------------------------------------ */

struct discover_ctx {
    char site_ids[32][64];
    char device_types[32][64];
    char device_ids[32][64];
    int  count;
};

static void discover_callback(const struct db_row *row, void *user_data)
{
    struct discover_ctx *ctx = (struct discover_ctx *)user_data;
    const char *sid = NULL, *dt = NULL, *did = NULL;
    int c;

    if (ctx->count >= 32) return;

    for (c = 0; c < row->field_count; c++) {
        const struct db_field *f = &row->fields[c];
        if (f->is_null) continue;
        if (strcmp(f->name, "site_id") == 0)      sid = f->value;
        else if (strcmp(f->name, "device_type") == 0) dt = f->value;
        else if (strcmp(f->name, "device_id") == 0)  did = f->value;
    }

    if (!sid || !dt || !did) return;

    /* De-duplicate */
    for (c = 0; c < ctx->count; c++) {
        if (strcmp(ctx->site_ids[c], sid)    == 0 &&
            strcmp(ctx->device_types[c], dt) == 0 &&
            strcmp(ctx->device_ids[c], did)  == 0)
            return;
    }

    snprintf(ctx->site_ids[ctx->count],     sizeof(ctx->site_ids[0]),     "%s", sid);
    snprintf(ctx->device_types[ctx->count], sizeof(ctx->device_types[0]), "%s", dt);
    snprintf(ctx->device_ids[ctx->count],   sizeof(ctx->device_ids[0]),   "%s", did);
    ctx->count++;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int opcua_server_init(const struct server_config   *server_cfg,
                      const struct security_config *sec_cfg,
                      const struct timescaledb_config *db_cfg)
{
    UA_ServerConfig *config;
    UA_StatusCode    rc;
    struct discover_ctx dctx;
    int i;

    (void)sec_cfg;
    (void)db_cfg;

    /* v1.4: create server with default config (listens on 0.0.0.0:4840) */
    g_server = UA_Server_new();
    if (!g_server) {
        fprintf(stderr, "opcua_server: UA_Server_new failed\n");
        return -1;
    }

    /* Customise application description */
    config = UA_Server_getConfig(g_server);
    UA_ApplicationDescription_clear(&config->applicationDescription);
    config->applicationDescription.applicationUri =
        UA_STRING_ALLOC(server_cfg->app_uri);
    config->applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC((char *)"", (char *)server_cfg->app_name);

    /* Build static tree */
    g_folder_root    = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    g_folder_edgevib = find_or_create_folder(g_folder_root, "EdgeVib");

    /* Discover devices */
    memset(&dctx, 0, sizeof(dctx));
    if (db_client_poll(
            "SELECT DISTINCT site_id, device_type, device_id "
            "FROM device_status_view",
            discover_callback, &dctx) != 0) {
        fprintf(stderr, "opcua_server: device discovery query failed\n");
    }

    for (i = 0; i < dctx.count; i++) {
        opcua_server_add_device(dctx.site_ids[i],
                                dctx.device_types[i],
                                dctx.device_ids[i]);
    }

    fprintf(stdout, "[%ld] INFO  opcua-srv: address space built, "
            "%d devices, %d variables\n",
            (long)time(NULL), dctx.count, node_mapping_count());

    /* Start */
    rc = UA_Server_run_startup(g_server);
    if (rc != UA_STATUSCODE_GOOD) {
        fprintf(stderr, "opcua_server: UA_Server_run_startup failed: %08x\n", rc);
        UA_Server_delete(g_server);
        g_server = NULL;
        return -1;
    }

    return 0;
}

UA_UInt32 opcua_server_iterate(void)
{
    if (!g_server) return 1000;
    return UA_Server_run_iterate(g_server, UA_FALSE);
}

/* Forward */
static void write_value_from_field(UA_UInt32 node_id,
                                   const struct db_field *field);

void opcua_server_update_values(const struct db_row *row)
{
    char        device_key[MAX_DEVICE_KEY];
    const char *site_id = NULL, *device_type = NULL, *device_id = NULL;
    int         c;

    if (!g_server) return;

    for (c = 0; c < row->field_count; c++) {
        const struct db_field *f = &row->fields[c];
        if (f->is_null) continue;
        if (strcmp(f->name, "site_id") == 0)      site_id     = f->value;
        else if (strcmp(f->name, "device_type") == 0) device_type = f->value;
        else if (strcmp(f->name, "device_id") == 0)  device_id   = f->value;
    }

    if (!site_id || !device_type || !device_id) return;

    snprintf(device_key, sizeof(device_key), "%s/%s/%s",
             site_id, device_type, device_id);

    for (c = 0; c < row->field_count; c++) {
        const struct db_field *f = &row->fields[c];
        UA_UInt32              nid;

        nid = node_mapping_find_node(device_key, f->name);
        if (nid == 0) continue;
        write_value_from_field(nid, f);
    }
}

void opcua_server_mark_all_bad(void)
{
    /* Stub — requires iterable mapping table.
     * For MVP: clients see stale values during DB outage.
     * TODO: add node_mapping_iter() to iterate all registered nodes. */
}

void opcua_server_deinit(void)
{
    if (g_server) {
        UA_Server_run_shutdown(g_server);
        UA_Server_delete(g_server);
        g_server = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Internal: write DB field value into OPC UA node                     */
/* ------------------------------------------------------------------ */

static void write_value_from_field(UA_UInt32 node_id,
                                   const struct db_field *field)
{
    UA_NodeId      nid;
    UA_Variant     var;
    UA_StatusCode  sc;

    nid = UA_NODEID_NUMERIC(1, node_id);

    if (field->is_null) {
        /* Leave last known value; no write */
        return;
    }

    if (strcmp(field->name, "last_ai_class") == 0 ||
        strcmp(field->name, "service_state") == 0) {
        UA_String ua_str = UA_STRING_ALLOC(field->value);
        UA_Variant_setScalar(&var, &ua_str, &UA_TYPES[UA_TYPES_STRING]);
        sc = UA_Server_writeValue(g_server, nid, var);
        UA_String_clear(&ua_str);
        (void)sc;

    } else if (strcmp(field->name, "data_quality") == 0) {
        UA_Int32 val = (UA_Int32)strtol(field->value, NULL, 10);
        UA_Variant_setScalar(&var, &val, &UA_TYPES[UA_TYPES_INT32]);
        sc = UA_Server_writeValue(g_server, nid, var);
        (void)sc;

    } else {
        UA_Double val = strtod(field->value, NULL);
        UA_Variant_setScalar(&var, &val, &UA_TYPES[UA_TYPES_DOUBLE]);
        sc = UA_Server_writeValue(g_server, nid, var);
        (void)sc;
    }
}
