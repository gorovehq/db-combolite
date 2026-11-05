/* Build profiles (spec 17): host (the default) or mcu, chosen at compile
 * time with CDB_PROFILE_MCU. Both read and write the same files; only the
 * limits on one running process differ (17.3), and where performance
 * depends on it the host profile grows working memory through the
 * environment's allocator (17.4). */
#ifndef CDB_PROFILE_H
#define CDB_PROFILE_H

#if !defined(CDB_PROFILE_MCU)
#define CDB_PROFILE_HOST 1
#endif

#ifdef CDB_PROFILE_HOST
#define CDB_PROFILE_NAME "host"
/* A query may grow through env alloc up to this, unless the database
 * configuration says otherwise (cdb_db_config.query_mem; spec 17.4) */
#define CDB_QUERY_MEM_DEFAULT (256u * 1024u * 1024u)
/* A neighbour-graph refresh may grow past vec_work up to this, unless the
 * database configuration says otherwise (cdb_db_config.vec_mem; spec 17.4).
 * Provisional: spec 17 leaves the default to measurement. */
#define CDB_VEC_MEM_DEFAULT (64u * 1024u * 1024u)
/* A request goes to the accelerator only when it reads at least this many
 * vector bytes, unless the configuration says otherwise
 * (cdb_db_config.accel_min_bytes; spec 18.4). Provisional: a device call
 * costs about 0.25 ms, the CPU's exact pass about that per 300 KB on the
 * development machine. */
#define CDB_ACCEL_MIN_BYTES_DEFAULT (1024u * 1024u)
#else
#define CDB_PROFILE_NAME "mcu"
#define CDB_QUERY_MEM_DEFAULT 0u /* no growth: caller memory only */
#define CDB_VEC_MEM_DEFAULT 0u
#endif

#endif /* CDB_PROFILE_H */
