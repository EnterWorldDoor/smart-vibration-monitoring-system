/**
 * @file db_client.c
 * @brief libpq thin wrapper — connection management + row iteration
 *
 * Design mirrors serial.c from rs232-gateway: open → poll → close lifecycle,
 * with recoverable-error retry driven by the main loop (never exit internally).
 */

#include "db_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

static void *g_conn = NULL;   /* PGconn *, hidden from callers */

/* Build a conninfo string from config */
static char *build_conninfo(const struct timescaledb_config *cfg)
{
    char *buf;
    int   n;

    buf = malloc(512);
    if (!buf) return NULL;

    n = snprintf(buf, 512,
        "host=%s port=%d dbname=%s user=%s password=%s "
        "connect_timeout=5",
        cfg->host, cfg->port, cfg->dbname, cfg->user, cfg->password);
    if (n < 0 || n >= 512) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* --- Public API ----------------------------------------------------------- */

int db_client_init(const struct timescaledb_config *cfg)
{
    char *conninfo;

    conninfo = build_conninfo(cfg);
    if (!conninfo) return -1;

    g_conn = PQconnectdb(conninfo);
    free(conninfo);

    if (PQstatus((PGconn *)g_conn) != CONNECTION_OK) {
        fprintf(stderr, "db_client: connection failed: %s\n",
                PQerrorMessage((PGconn *)g_conn));
        PQfinish((PGconn *)g_conn);
        g_conn = NULL;
        return -1;
    }
    return 0;
}

void db_client_deinit(void)
{
    if (g_conn) {
        PQfinish((PGconn *)g_conn);
        g_conn = NULL;
    }
}

int db_client_poll(const char *query, db_row_callback_t cb, void *user_data)
{
    PGresult *res;
    int       nrows, ncols;
    int       r, c;

    if (!g_conn) return -1;

    res = PQexec((PGconn *)g_conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "db_client: query failed: %s\n",
                PQerrorMessage((PGconn *)g_conn));
        PQclear(res);
        return -1;
    }

    nrows = PQntuples(res);
    ncols = PQnfields(res);

    for (r = 0; r < nrows; r++) {
        struct db_row row;

        if (ncols > DB_MAX_COLUMNS)
            ncols = DB_MAX_COLUMNS;

        row.field_count = ncols;

        for (c = 0; c < ncols; c++) {
            struct db_field *f = &row.fields[c];

            snprintf(f->name, sizeof(f->name), "%s", PQfname(res, c));
            f->is_null = PQgetisnull(res, r, c);
            if (f->is_null) {
                f->value[0] = '\0';
            } else {
                snprintf(f->value, sizeof(f->value), "%s",
                         PQgetvalue(res, r, c));
            }
        }

        if (cb)
            cb(&row, user_data);
    }

    PQclear(res);
    return 0;
}

int db_client_is_connected(void)
{
    if (!g_conn) return 0;
    return PQstatus((PGconn *)g_conn) == CONNECTION_OK;
}

int db_client_reconnect(void)
{
    if (!g_conn) return -1;
    PQreset((PGconn *)g_conn);
    if (PQstatus((PGconn *)g_conn) != CONNECTION_OK) {
        fprintf(stderr, "db_client: reconnect failed: %s\n",
                PQerrorMessage((PGconn *)g_conn));
        return -1;
    }
    return 0;
}
