#ifndef __plexor_h__
#define __plexor_h__

#include <postgres.h>
#include <commands/trigger.h>
#include <commands/defrem.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_foreign_data_wrapper.h>
#include <catalog/pg_user_mapping.h>
#include <catalog/pg_namespace.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <access/htup_details.h>
#include <access/reloptions.h>
#include <access/hash.h>
#include <access/xact.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
#include <utils/memutils.h>
#include <utils/acl.h>
#include <executor/spi.h>
#include <foreign/foreign.h>
#include <lib/stringinfo.h>
#include <sys/epoll.h>
#include <funcapi.h>
#include <libpq-fe.h>
#include <miscadmin.h>

/*
longest string of {auto commit | {read committed | serializable } [read write | read only] [ [ not ] deferrable] }
is "read committed read write not deferrable" (42 chars)
*/
#define MAX_ISOLATION_LEVEL_LEN 42
#define MAX_DSN_LEN 1024
#define MAX_RESULTS_PER_EXPR 128
#define MAX_CONNECTIONS 128
#define TYPED_SQL_TMPL "select %s"
#define UNTYPED_SQL_TMPL "select x from (select * from %s as (%s)) as x"

/* tuple stamp */
typedef struct TupleStamp {
    TransactionId   xmin;
    ItemPointerData tid;
} TupleStamp;

static inline void
plx_set_stamp(TupleStamp *stamp, HeapTuple tuple)
{
    stamp->xmin = HeapTupleHeaderGetXmin(tuple->t_data);
    stamp->tid = tuple->t_self;
}

static inline bool
plx_check_stamp(TupleStamp *stamp, HeapTuple tuple)
{
    return stamp->xmin == HeapTupleHeaderGetXmin(tuple->t_data)
           && ItemPointerEquals(&stamp->tid, &tuple->t_self);
}

/* Copy string using specified context */
static inline char *
mctx_strcpy(MemoryContext mctx, const char *s)
{
    int   len = strlen(s) + 1;
    return memcpy(MemoryContextAlloc(mctx, len), s, len);
}


typedef struct PlxCluster
{
    Oid             oid;                     /* foreign server OID  */
    char            name[NAMEDATALEN];       /* foreign server name */
    char           *isolation_level;
    int             connection_lifetime;
    char            nodes[100][MAX_DSN_LEN]; /* node DSNs           */
    int             nnodes;                  /* nodes count         */
} PlxCluster;


typedef struct PlxType
{
    Oid             oid;                     /* type OID */
    FmgrInfo        send_fn;                 /* OID of binary out convert procedure  */
    FmgrInfo        receive_fn;              /* OID of binary in  convert procedure  */
    FmgrInfo        output_fn;               /* OID of text   out convert procedure  */
    FmgrInfo        input_fn;                /* OID of text   in  convert procedure  */
    Oid             receive_io_params;       /* OID to pass to I/O convert procedure */
    TupleStamp      stamp;                   /* stamp to check type up to date       */
} PlxType;


typedef struct PlxQuery
{
    StringInfo      sql;                     /* sql that contain query                   */
    int            *plx_fn_arg_indexes;      /* indexes of plx_fn that use this PlxQuery */
    int             nargs;                   /* plx_fn_arg_indexes len                   */
} PlxQuery;


typedef enum RunOnType
{
    RUN_ON_HASH  = 1,                        /* node returned by hash function         */
    RUN_ON_NNODE = 2,                        /* exact node number                      */
    RUN_ON_ANY   = 3,                        /* decide randomly during runtime         */
    RUN_ON_ANODE = 4,                        /* get node number from function argument */
} RunOnType;

typedef struct PlxFn
{
    MemoryContext   mctx;                    /* function MemoryContext                     */
    Oid             oid;                     /* function OID                               */
    char           *name;                    /* function name                              */
    char           *cluster_name;            /* cluster name at which "run on" function
                                                will be called                             */
    RunOnType       run_on;                  /* type of method to find node to run on      */
    int             nnode;                   /* node number (RUN_ON_NNODE)                 */
    int             anode;                   /* argument index that contain node number
                                                (RUN_ON_ANODE)                             */
    PlxQuery       *hash_query;              /* query to find node to run on (RUN_ON_HASH) */
    PlxQuery       *run_query;
    PlxType       **arg_types;
    char          **arg_names;
    int             nargs;
    PlxType        *ret_type;
    int             ret_type_mod;
    bool            is_binary;
    bool            is_untyped_record;       /* return type is untyped record */
    TupleStamp      stamp;
} PlxFn;

typedef struct PlxResult
{
    PlxFn          *plx_fn;
    PGresult       *pg_result;
} PlxResult;

typedef struct PlxConn
{
    PlxCluster     *plx_cluster;
    PGconn         *pq_conn;
    char           *dsn;
    int             xlevel;                  /* transaction nest level */
    time_t          connect_time;
} PlxConn;

/* Structure to keep plx_conn in HTAB's context. */
typedef struct PlxConnHashEntry
{
    /* Key value. Must be at the start */
    char            key[MAX_DSN_LEN];
    /* Pointer to connection data */
    PlxConn        *plx_conn;
} PlxConnHashEntry;


/* cluster.c */

void        plx_cluster_cache_init(void);
PlxCluster *get_plx_cluster(char* name);
void        delete_plx_cluster(PlxCluster *plx_cluster);
bool        extract_node_num(const char *node_name, int *node_num);

/* type.c */
bool     is_plx_type_todate(PlxType *plx_type);
PlxType *new_plx_type(Oid oid, MemoryContext mctx);


/* query.c */
PlxQuery *new_plx_query(MemoryContext mctx);
void      delete_plx_query(PlxQuery *plx_q);
void      append_plx_query_arg_index(PlxQuery *plx_q, PlxFn *plx_fn, const char *name);
PlxQuery *create_plx_query_from_plx_fn(PlxFn *plx_fn);

/* function.c*/
void   plx_fn_cache_init(void);
PlxFn *compile_plx_fn(FunctionCallInfo fcinfo, HeapTuple proc_tuple, bool is_validate);
PlxFn *get_plx_fn(FunctionCallInfo fcinfo);
PlxFn *plx_fn_lookup_cache(Oid fn_oid);
void   delete_plx_fn(PlxFn *plx_fn, bool is_cache_delete);
void   fill_plx_fn_cluster_name(PlxFn* plx_fn, const char *cluster_name);
void   fill_plx_fn_anode(PlxFn* plx_fn, const char *anode_name);
int    plx_fn_get_arg_index(PlxFn *plx_fn, const char *name);


/* result.c */
void  plx_result_cache_init(void);
void  plx_result_insert_cache(FunctionCallInfo fcinfo, PlxFn *plx_fn, PGresult *pg_result);
void  set_single_result(PlxFn *plx_fn, PGresult* pg_result);
Datum get_single_result(FunctionCallInfo fcinfo);
Datum get_next_row(FunctionCallInfo fcinfo);


/* connection.c */
void     plx_conn_cache_init(void);
PlxConn *get_plx_conn(PlxCluster *plx_cluster, int nnode);
void     delete_plx_conn(PlxConn *plx_conn);


/* transaction.c */
void start_transaction(PlxConn* plx_conn);


/* plexor.c */
void plx_error_with_errcode(PlxFn *plx_fn, int err_code, const char *fmt, ...)
	__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 4)));
#define plx_error(func,...) plx_error_with_errcode((func), ERRCODE_INTERNAL_ERROR, __VA_ARGS__)

void plexor_yyerror(const char *fmt, ...)
	__attribute__((format(PG_PRINTF_ATTRIBUTE, 1, 2)));

void execute_init(void);
void remote_execute(PlxConn *plx_conn, PlxFn *plx_fn, FunctionCallInfo fcinfo);


/* scanner.l */
void plexor_yylex_prepare(void);


/* parser.y */
void run_plexor_parser(PlxFn *plx_fn, const char *body, int len);

#endif