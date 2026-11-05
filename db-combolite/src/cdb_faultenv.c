/* Fault-injecting environment (cdb_faultenv.h). Test-only.
 *
 * Each file and directory has two states: what the running process sees
 * (exists, name) and what would survive a crash (durable_exists,
 * durable_name). Creates, removes and renames change only the first;
 * syncing the parent directory copies it to the second. At reboot after a
 * crash, the durable state wins. File contents are handled by each file's
 * fault device, on the shared crash clock. */
#include <stdlib.h>
#include <string.h>

#include "cdb_faultenv.h"

/* Parent directory of a path; "." for a bare name. */
static void parent_of(const char *path, char *out)
{
    const char *slash = strrchr(path, '/');
    size_t n;
    if (slash == NULL) {
        strcpy(out, ".");
        return;
    }
    n = (size_t)(slash - path);
    memcpy(out, path, n);
    out[n] = '\0';
}

static int name_ok(const char *path)
{
    return path != NULL && path[0] != '\0' && strlen(path) < CDB_FAULTENV_NAME;
}

static cdb_faultenv_file *find_file(cdb_faultenv *fe, const char *path)
{
    int i;
    for (i = 0; i < CDB_FAULTENV_MAX_FILES; i++)
        if (fe->files[i].exists && strcmp(fe->files[i].name, path) == 0)
            return &fe->files[i];
    return NULL;
}

static cdb_faultenv_dir *find_dir(cdb_faultenv *fe, const char *path)
{
    int i;
    for (i = 0; i < CDB_FAULTENV_MAX_DIRS; i++)
        if (fe->dirs[i].exists && strcmp(fe->dirs[i].name, path) == 0)
            return &fe->dirs[i];
    return NULL;
}

static int dir_present(cdb_faultenv *fe, const char *path)
{
    return strcmp(path, ".") == 0 || find_dir(fe, path) != NULL;
}

/* A file slot free in both states (a removed file still holds its slot
 * until the removal is durable, since a crash could bring it back). */
static int file_slot(cdb_faultenv *fe)
{
    int i;
    for (i = 0; i < CDB_FAULTENV_MAX_FILES; i++)
        if (!fe->files[i].exists && !fe->files[i].durable_exists)
            return i;
    return -1;
}

/* Opening an existing file rebinds its device (contents kept). Creating
 * one needs its parent directory to exist, counts as a crash point, and
 * gives the file a fresh device carved from the arena: current image,
 * durable image and log data, one file_capacity each. */
static cdb_status fe_open(void *ctx, const char *path, unsigned flags, cdb_storage *out)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    cdb_faultenv_file *f;
    char parent[CDB_FAULTENV_NAME];
    cdb_faultdev_config dc;
    uint8_t *base;
    int slot;

    if (fe->clock.crashed)
        return CDB_E_IO;
    if (!name_ok(path))
        return CDB_E_INVALID_ARG;
    if ((f = find_file(fe, path)) != NULL)
        return cdb_faultdev_rebind(&f->dev, out); /* keep its contents */
    if (!(flags & CDB_ENV_CREATE))
        return CDB_E_NOT_FOUND;
    parent_of(path, parent);
    if (!dir_present(fe, parent))
        return CDB_E_NOT_FOUND;
    if ((slot = file_slot(fe)) < 0)
        return CDB_E_BUFFER_TOO_SMALL;
    if (cdb_fault_clock_tick(&fe->clock))
        return CDB_E_IO; /* the crash struck the create itself */

    f = &fe->files[slot];
    memset(f, 0, sizeof *f);
    strcpy(f->name, path);
    f->exists = 1;
    base = fe->cfg.arena + (size_t)slot * 3u * fe->cfg.file_capacity;
    memset(&dc, 0, sizeof dc);
    dc.current = base;
    dc.durable = base + fe->cfg.file_capacity;
    dc.log_data = base + 2u * fe->cfg.file_capacity;
    dc.capacity = fe->cfg.file_capacity;
    dc.log_data_cap = fe->cfg.file_capacity;
    dc.log = fe->cfg.records + (size_t)slot * fe->cfg.records_per_file;
    dc.log_cap = fe->cfg.records_per_file;
    dc.geometry.program_unit = 1;
    dc.clock = &fe->clock;
    return cdb_faultdev_init(&f->dev, &dc, out);
}

static cdb_status fe_remove(void *ctx, const char *path)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    cdb_faultenv_file *f;
    if (fe->clock.crashed)
        return CDB_E_IO;
    if ((f = find_file(fe, path)) == NULL)
        return CDB_E_NOT_FOUND;
    if (cdb_fault_clock_tick(&fe->clock))
        return CDB_E_IO;
    f->exists = 0;
    return CDB_OK;
}

static cdb_status fe_rename(void *ctx, const char *from, const char *to)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    cdb_faultenv_file *f, *target;
    char pf[CDB_FAULTENV_NAME], pt[CDB_FAULTENV_NAME];
    if (fe->clock.crashed)
        return CDB_E_IO;
    if (!name_ok(to))
        return CDB_E_INVALID_ARG;
    if ((f = find_file(fe, from)) == NULL)
        return CDB_E_NOT_FOUND;
    parent_of(from, pf);
    parent_of(to, pt);
    if (strcmp(pf, pt) != 0)
        return CDB_E_UNSUPPORTED; /* renames stay within one directory */
    if (cdb_fault_clock_tick(&fe->clock))
        return CDB_E_IO;
    if ((target = find_file(fe, to)) != NULL && target != f)
        target->exists = 0; /* atomically replaced */
    strcpy(f->name, to);
    return CDB_OK;
}

static cdb_status fe_mkdir(void *ctx, const char *path)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    char parent[CDB_FAULTENV_NAME];
    int i;
    if (fe->clock.crashed)
        return CDB_E_IO;
    if (!name_ok(path))
        return CDB_E_INVALID_ARG;
    if (find_dir(fe, path))
        return CDB_OK;
    parent_of(path, parent);
    if (!dir_present(fe, parent))
        return CDB_E_NOT_FOUND;
    for (i = 0; i < CDB_FAULTENV_MAX_DIRS; i++)
        if (!fe->dirs[i].exists && !fe->dirs[i].durable_exists)
            break;
    if (i == CDB_FAULTENV_MAX_DIRS)
        return CDB_E_BUFFER_TOO_SMALL;
    if (cdb_fault_clock_tick(&fe->clock))
        return CDB_E_IO;
    memset(&fe->dirs[i], 0, sizeof fe->dirs[i]);
    strcpy(fe->dirs[i].name, path);
    fe->dirs[i].exists = 1;
    return CDB_OK;
}

static cdb_status fe_rmdir(void *ctx, const char *path)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    cdb_faultenv_dir *d;
    char parent[CDB_FAULTENV_NAME];
    int i;
    if (fe->clock.crashed)
        return CDB_E_IO;
    if ((d = find_dir(fe, path)) == NULL)
        return CDB_E_NOT_FOUND;
    for (i = 0; i < CDB_FAULTENV_MAX_FILES; i++) {
        if (!fe->files[i].exists)
            continue;
        parent_of(fe->files[i].name, parent);
        if (strcmp(parent, path) == 0)
            return CDB_E_STATE; /* not empty */
    }
    if (cdb_fault_clock_tick(&fe->clock))
        return CDB_E_IO;
    d->exists = 0;
    return CDB_OK;
}

static cdb_status fe_exists(void *ctx, const char *path, int *exists)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    if (fe->clock.crashed)
        return CDB_E_IO;
    *exists = find_file(fe, path) != NULL || find_dir(fe, path) != NULL;
    return CDB_OK;
}

/* Makes the directory's entries durable: every file or subdirectory whose
 * current or durable name lies directly in it gets its current state
 * copied to its durable state. */
static cdb_status fe_sync_dir(void *ctx, const char *path)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    char parent[CDB_FAULTENV_NAME];
    int i;
    if (fe->clock.crashed)
        return CDB_E_IO;
    if (!dir_present(fe, path))
        return CDB_E_NOT_FOUND;
    if (cdb_fault_clock_tick(&fe->clock))
        return CDB_E_IO;
    for (i = 0; i < CDB_FAULTENV_MAX_FILES; i++) {
        cdb_faultenv_file *f = &fe->files[i];
        int touched = 0;
        if (f->exists) {
            parent_of(f->name, parent);
            touched |= strcmp(parent, path) == 0;
        }
        if (f->durable_exists) {
            parent_of(f->durable_name, parent);
            touched |= strcmp(parent, path) == 0;
        }
        if (touched) {
            f->durable_exists = f->exists;
            strcpy(f->durable_name, f->name);
        }
    }
    for (i = 0; i < CDB_FAULTENV_MAX_DIRS; i++) {
        cdb_faultenv_dir *d = &fe->dirs[i];
        parent_of(d->name, parent);
        if ((d->exists || d->durable_exists) && strcmp(parent, path) == 0)
            d->durable_exists = d->exists;
    }
    return CDB_OK;
}

static cdb_status fe_now(void *ctx, int64_t *ns)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    if (!fe->has_time)
        return CDB_E_UNSUPPORTED;
    *ns = fe->time_ns;
    return CDB_OK;
}

/* The allocator: malloc, with every live block remembered (up to
 * CDB_FAULTENV_MAX_ALLOCS) so a reboot can free them and tests can count
 * leaks. Returns NULL when switched off or full. */
static void *fe_alloc(void *ctx, size_t size)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    void *p;
    if (fe->no_alloc || fe->live == CDB_FAULTENV_MAX_ALLOCS || (p = malloc(size)) == NULL)
        return NULL;
    fe->allocs[fe->live++] = p;
    return p;
}

static void fe_free(void *ctx, void *p)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    unsigned i;
    for (i = 0; i < fe->live; i++)
        if (fe->allocs[i] == p) {
            fe->allocs[i] = fe->allocs[--fe->live];
            free(p);
            return;
        }
}

/* A crash or shutdown ends the process, and its memory with it. */
static void free_all(cdb_faultenv *fe)
{
    while (fe->live)
        free(fe->allocs[--fe->live]);
}

/* The files and directories whose parent is dir, as the running process
 * sees them. */
static cdb_status fe_list(void *ctx, const char *dir, int (*fn)(void *arg, const char *name), void *arg)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    char parent[CDB_FAULTENV_NAME];
    size_t n = strlen(dir);
    int i;
    if (fe->clock.crashed)
        return CDB_E_IO;
    if (!dir_present(fe, dir))
        return CDB_E_NOT_FOUND;
    for (i = 0; i < CDB_FAULTENV_MAX_FILES; i++) {
        if (!fe->files[i].exists)
            continue;
        parent_of(fe->files[i].name, parent);
        if (strcmp(parent, dir) == 0 && fn(arg, fe->files[i].name + n + 1))
            return CDB_OK;
    }
    for (i = 0; i < CDB_FAULTENV_MAX_DIRS; i++) {
        if (!fe->dirs[i].exists)
            continue;
        parent_of(fe->dirs[i].name, parent);
        if (strcmp(parent, dir) == 0 && fn(arg, fe->dirs[i].name + n + 1))
            return CDB_OK;
    }
    return CDB_OK;
}

static cdb_status fe_mono(void *ctx, int64_t *ns)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    *ns = fe->mono_ns;
    return CDB_OK;
}

static cdb_status fe_random(void *ctx, void *buf, size_t n)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    uint8_t *p = (uint8_t *)buf;
    while (n--) {
        fe->rng ^= fe->rng >> 12;
        fe->rng ^= fe->rng << 25;
        fe->rng ^= fe->rng >> 27;
        *p++ = (uint8_t)((fe->rng * 0x2545F4914F6CDD1Dull) >> 56);
    }
    return CDB_OK;
}

static void fe_sleep(void *ctx, int64_t ns)
{
    cdb_faultenv *fe = (cdb_faultenv *)ctx;
    cdb_faultenv_advance(fe, ns);
    if (fe->on_sleep != NULL && !fe->in_sleep_hook) {
        fe->in_sleep_hook = 1;
        fe->on_sleep(fe->on_sleep_arg);
        fe->in_sleep_hook = 0;
    }
}

void cdb_faultenv_on_sleep(cdb_faultenv *fe, void (*fn)(void *arg), void *arg)
{
    fe->on_sleep = fn;
    fe->on_sleep_arg = arg;
}

static const cdb_env_ops faultenv_ops = {
    fe_open, fe_remove, fe_rename, fe_mkdir, fe_rmdir, fe_exists, fe_sync_dir, fe_now, fe_alloc, fe_free,
    fe_list, fe_mono, fe_random, fe_sleep,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL
};

void cdb_faultenv_advance(cdb_faultenv *fe, int64_t ns)
{
    if (ns > 0)
        fe->mono_ns += ns;
}

void cdb_faultenv_set_time(cdb_faultenv *fe, int on, int64_t ns)
{
    fe->has_time = on;
    fe->time_ns = ns;
}

void cdb_faultenv_set_alloc(cdb_faultenv *fe, int on)
{
    fe->no_alloc = !on;
}

unsigned cdb_faultenv_live_allocs(const cdb_faultenv *fe)
{
    return fe->live;
}

cdb_status cdb_faultenv_init(cdb_faultenv *fe, const cdb_faultenv_config *cfg, cdb_env *out)
{
    if (fe == NULL || cfg == NULL || out == NULL || cfg->arena == NULL || cfg->records == NULL)
        return CDB_E_INVALID_ARG;
    memset(fe, 0, sizeof *fe);
    fe->cfg = *cfg;
    fe->mono_ns = 1000000000;
    fe->rng = 0x9E3779B97F4A7C15ull;
    out->ops = &faultenv_ops;
    out->ctx = fe;
    out->accel = NULL;
    out->accel_ctx = NULL;
    return CDB_OK;
}

void cdb_faultenv_arm(cdb_faultenv *fe, uint64_t op, cdb_crash_mode mode,
                      size_t keep_writes, size_t torn_bytes)
{
    fe->clock.crash_at = op == 0 ? 0 : fe->clock.ops + op;
    fe->clock.mode = mode;
    fe->clock.keep_writes = keep_writes;
    fe->clock.torn_bytes = torn_bytes;
}

void cdb_faultenv_crash_now(cdb_faultenv *fe, cdb_crash_mode mode,
                            size_t keep_writes, size_t torn_bytes)
{
    fe->clock.crashed = 1;
    fe->clock.mode = mode;
    fe->clock.keep_writes = keep_writes;
    fe->clock.torn_bytes = torn_bytes;
}

void cdb_faultenv_reboot(cdb_faultenv *fe)
{
    char parent[CDB_FAULTENV_NAME];
    int crashed = fe->clock.crashed;
    int i;

    /* Without a crash every current state is kept (an orderly shutdown);
     * after one, only the durable state survives. Directories first, so a
     * file whose directory was lost is lost too. */
    free_all(fe);
    for (i = 0; i < CDB_FAULTENV_MAX_DIRS; i++) {
        cdb_faultenv_dir *d = &fe->dirs[i];
        if (!crashed)
            d->durable_exists = d->exists;
        d->exists = d->durable_exists;
    }
    for (i = 0; i < CDB_FAULTENV_MAX_FILES; i++) {
        cdb_faultenv_file *f = &fe->files[i];
        if (!crashed) {
            f->durable_exists = f->exists;
            strcpy(f->durable_name, f->name);
        }
        if (f->durable_exists) {
            parent_of(f->durable_name, parent);
            if (!dir_present(fe, parent))
                f->durable_exists = 0; /* its directory never became durable */
        }
        f->exists = f->durable_exists;
        if (f->exists) {
            strcpy(f->name, f->durable_name);
            cdb_faultdev_reboot(&f->dev);
        }
    }
    fe->clock.crashed = 0;
    fe->clock.crash_at = 0;
}

cdb_faultdev *cdb_faultenv_device(cdb_faultenv *fe, const char *path)
{
    cdb_faultenv_file *f = find_file(fe, path);
    return f ? &f->dev : NULL;
}
