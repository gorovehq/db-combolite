/* Full-text search (spec 8.10, 14.14). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_fts.h"
#include "cdb_key.h"

#define BM25_K1 1.2
#define BM25_B 0.75
#define KEY_MAX 96u

/* Settings fields (spec 14.14). */
enum { S_TERMS = 1, S_POSTINGS, S_LENGTHS, S_SPLITTER, S_SUMMARY, S_PATHS };

size_t cdb_fts_buffers_size(size_t page_size, size_t doc_max)
{
    return cdb_doc_buffer_size(page_size, doc_max) + 2u * page_size;
}

size_t cdb_fts_tokens_needed(size_t page_size, size_t doc_max)
{
    return doc_max ? 2u * (doc_max + page_size) : 2u * page_size;
}

/* fts_buffers: a document's earlier body and summary (cdb_doc), then the
 * cursor page. */
static uint8_t *cursor_page(cdb_db *db)
{
    return db->cfg.fts_buffers + cdb_doc_buffer_size(db->pager.page_size, db->cfg.doc_max) + db->pager.page_size;
}

/* -------------------------------------------------------- words */

/* Decodes one UTF-8 character; malformed bytes count as one character
 * that is part of a word, so any input splits deterministically. */
static size_t decode_cp(const uint8_t *p, size_t len, uint32_t *cp)
{
    uint8_t b = p[0];
    size_t n, i;
    uint32_t c;
    if (b < 0x80u) {
        *cp = b;
        return 1;
    }
    if (b >= 0xF0u && b < 0xF8u) {
        n = 4;
        c = b & 0x07u;
    } else if (b >= 0xE0u && b < 0xF0u) {
        n = 3;
        c = b & 0x0Fu;
    } else if (b >= 0xC0u && b < 0xE0u) {
        n = 2;
        c = b & 0x1Fu;
    } else {
        *cp = 0xFFFDu;
        return 1;
    }
    if (n > len) {
        *cp = 0xFFFDu;
        return 1;
    }
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            *cp = 0xFFFDu;
            return 1;
        }
        c = (c << 6) | (p[i] & 0x3Fu);
    }
    *cp = c;
    return n;
}

static int is_separator(uint32_t cp)
{
    if (cp < 0x80u)
        return !((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'));
    if (cp <= 0xBFu)  /* Latin-1 controls, no-break space, punctuation, symbols */
        return cp != 0xAAu && cp != 0xB5u && cp != 0xBAu;
    if (cp == 0xD7u || cp == 0xF7u)
        return 1;
    if (cp >= 0x2000u && cp <= 0x206Fu) /* general punctuation and spaces */
        return 1;
    if (cp >= 0x3000u && cp <= 0x303Fu) /* CJK punctuation and space */
        return 1;
    return cp == 0xFEFFu;
}

size_t cdb_fts_split(const uint8_t *text, size_t len, cdb_fts_token *out, size_t cap, size_t *n)
{
    size_t i = 0, found = 0;
    while (i < len) {
        uint32_t cp;
        size_t step = decode_cp(text + i, len - i, &cp);
        size_t start, wlen = 0;
        if (is_separator(cp)) {
            i += step;
            continue;
        }
        start = i;
        while (i < len) {
            step = decode_cp(text + i, len - i, &cp);
            if (is_separator(cp))
                break;
            if (i + step - start <= CDB_FTS_MAX_WORD)
                wlen = i + step - start; /* cut on a character boundary */
            i += step;
        }
        if (*n < cap) {
            out[*n].p = text + start;
            out[*n].len = (uint32_t)wlen;
            out[*n].tf = 1;
            (*n)++;
        }
        found++;
    }
    return found;
}

static uint8_t fold(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c;
}

int cdb_fts_word_cmp(const cdb_fts_token *a, const cdb_fts_token *b)
{
    uint32_t i, n = a->len < b->len ? a->len : b->len;
    for (i = 0; i < n; i++) {
        uint8_t x = fold(a->p[i]), y = fold(b->p[i]);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

static int qsort_cmp(const void *a, const void *b)
{
    return cdb_fts_word_cmp((const cdb_fts_token *)a, (const cdb_fts_token *)b);
}

/* Sorts words and merges repeats, adding up tf. Returns the unique count. */
static size_t group(cdb_fts_token *t, size_t n)
{
    size_t i, u = 0;
    if (n == 0)
        return 0;
    qsort(t, n, sizeof *t, qsort_cmp);
    for (i = 1; i < n; i++) {
        if (cdb_fts_word_cmp(&t[u], &t[i]) == 0)
            t[u].tf += t[i].tf;
        else
            t[++u] = t[i];
    }
    return u + 1;
}

static void split_value(const cdb_value *v, cdb_byte_order o, cdb_fts_token *out, size_t cap, size_t *n)
{
    if (v->type == CDB_TYPE_STRING) {
        (void)cdb_fts_split(v->payload, v->payload_len, out, cap, n);
    } else if (v->type == CDB_TYPE_ARRAY) {
        cdb_value_iter it;
        cdb_value e;
        int done;
        if (cdb_value_array_iter(v, o, &it) != CDB_OK)
            return;
        while (cdb_value_array_next(&it, &e, &done) == CDB_OK && !done)
            if (e.type == CDB_TYPE_STRING)
                (void)cdb_fts_split(e.payload, e.payload_len, out, cap, n);
    }
}

cdb_status cdb_fts_collect(const cdb_fts_config *cfg, const cdb_fts_doc *d, cdb_byte_order order,
                           cdb_fts_token *out, size_t cap, size_t *unique, size_t *total)
{
    cdb_value root;
    size_t n = 0;
    unsigned p, i;

    *unique = *total = 0;
    if (d->body == NULL)
        return CDB_OK;
    if (cdb_value_read(d->body, d->body_len, order, &root) != CDB_OK || root.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    for (p = 0; p < cfg->npaths; p++) {
        cdb_value v = root;
        int found = 1;
        for (i = 0; i < cfg->paths[p].depth && found; i++) {
            cdb_value next;
            if (v.type != CDB_TYPE_OBJECT || cdb_value_object_get(&v, order, cfg->paths[p].ids[i], &next, &found) != CDB_OK)
                found = 0;
            else if (found)
                v = next;
        }
        if (found)
            split_value(&v, order, out, cap, &n);
    }
    if (cfg->summary && d->summary)
        (void)cdb_fts_split(d->summary, d->summary_len, out, cap, &n);
    *total = n;
    *unique = group(out, n);
    return CDB_OK;
}

/* -------------------------------------------------------- settings */

cdb_status cdb_fts_check_config(const cdb_fts_config *cfg)
{
    unsigned p;
    if (cfg == NULL || cfg->npaths > CDB_FTS_MAX_PATHS || (cfg->npaths == 0 && !cfg->summary))
        return CDB_E_INVALID_ARG;
    for (p = 0; p < cfg->npaths; p++)
        if (cfg->paths[p].depth == 0 || cfg->paths[p].depth > CDB_FTS_MAX_DEPTH)
            return CDB_E_INVALID_ARG;
    return CDB_OK;
}

cdb_status cdb_fts_encode(const cdb_fts_config *cfg, uint64_t terms, uint64_t postings, uint64_t lengths,
                          cdb_byte_order order, uint8_t *out, size_t cap, size_t *len)
{
    cdb_value_writer w;
    unsigned p, i;
    cdb_value_writer_init(&w, out, cap, order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, S_TERMS);
    cdb_value_write_int(&w, (int64_t)terms);
    cdb_value_field(&w, S_POSTINGS);
    cdb_value_write_int(&w, (int64_t)postings);
    cdb_value_field(&w, S_LENGTHS);
    cdb_value_write_int(&w, (int64_t)lengths);
    cdb_value_field(&w, S_SPLITTER);
    cdb_value_write_int(&w, CDB_FTS_SPLITTER_SIMPLE);
    cdb_value_field(&w, S_SUMMARY);
    cdb_value_write_bool(&w, cfg->summary != 0);
    cdb_value_field(&w, S_PATHS);
    cdb_value_begin_array(&w);
    for (p = 0; p < cfg->npaths; p++) {
        cdb_value_begin_array(&w);
        for (i = 0; i < cfg->paths[p].depth; i++)
            cdb_value_write_int(&w, (int64_t)cfg->paths[p].ids[i]);
        cdb_value_end_array(&w);
    }
    cdb_value_end_array(&w);
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

static cdb_status get_field(const cdb_value *obj, cdb_byte_order o, uint64_t f, cdb_value *v)
{
    int found;
    if (cdb_value_object_get(obj, o, f, v, &found) != CDB_OK || !found)
        return CDB_E_CORRUPT;
    return CDB_OK;
}

static cdb_status get_id(const cdb_value *obj, cdb_byte_order o, uint64_t f, uint64_t *out)
{
    cdb_value v;
    if (get_field(obj, o, f, &v) != CDB_OK || v.type != CDB_TYPE_INT || v.as.i64 <= 0)
        return CDB_E_CORRUPT;
    *out = (uint64_t)v.as.i64;
    return CDB_OK;
}

cdb_status cdb_fts_decode(const uint8_t *in, size_t len, cdb_byte_order order, cdb_fts_config *cfg,
                          uint64_t *terms, uint64_t *postings, uint64_t *lengths)
{
    cdb_value obj, v, path, seg;
    cdb_value_iter it, pi;
    int done;

    memset(cfg, 0, sizeof *cfg);
    if (cdb_value_read(in, len, order, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if (get_id(&obj, order, S_TERMS, terms) != CDB_OK || get_id(&obj, order, S_POSTINGS, postings) != CDB_OK
        || get_id(&obj, order, S_LENGTHS, lengths) != CDB_OK)
        return CDB_E_CORRUPT;
    if (get_field(&obj, order, S_SPLITTER, &v) != CDB_OK || v.type != CDB_TYPE_INT)
        return CDB_E_CORRUPT;
    if (v.as.i64 != CDB_FTS_SPLITTER_SIMPLE)
        return CDB_E_UNSUPPORTED; /* e.g. ICU, in a build without it */
    if (get_field(&obj, order, S_SUMMARY, &v) != CDB_OK || v.type != CDB_TYPE_BOOL)
        return CDB_E_CORRUPT;
    cfg->summary = v.as.boolean;
    if (get_field(&obj, order, S_PATHS, &v) != CDB_OK || v.type != CDB_TYPE_ARRAY
        || cdb_value_array_iter(&v, order, &it) != CDB_OK)
        return CDB_E_CORRUPT;
    for (;;) {
        cdb_fts_path *fp;
        if (cdb_value_array_next(&it, &path, &done) != CDB_OK)
            return CDB_E_CORRUPT;
        if (done)
            break;
        if (cfg->npaths >= CDB_FTS_MAX_PATHS || path.type != CDB_TYPE_ARRAY
            || cdb_value_array_iter(&path, order, &pi) != CDB_OK)
            return CDB_E_CORRUPT;
        fp = &cfg->paths[cfg->npaths++];
        for (;;) {
            if (cdb_value_array_next(&pi, &seg, &done) != CDB_OK)
                return CDB_E_CORRUPT;
            if (done)
                break;
            if (fp->depth >= CDB_FTS_MAX_DEPTH || seg.type != CDB_TYPE_INT || seg.as.i64 < 0)
                return CDB_E_CORRUPT;
            fp->ids[fp->depth++] = (uint64_t)seg.as.i64;
        }
    }
    return cdb_fts_check_config(cfg) == CDB_OK ? CDB_OK : CDB_E_CORRUPT;
}

/* -------------------------------------------------------- index records */

static size_t term_key(uint8_t *k, const cdb_fts_token *t, int with_entry, uint64_t entry)
{
    char word[CDB_FTS_MAX_WORD];
    cdb_key_builder kb;
    uint32_t i;
    for (i = 0; i < t->len; i++)
        word[i] = (char)fold(t->p[i]);
    cdb_key_init(&kb, k, KEY_MAX);
    cdb_key_string(&kb, word, t->len);
    if (with_entry)
        cdb_key_u64(&kb, entry);
    return kb.len;
}

static size_t entry_key(uint8_t *k, uint64_t entry)
{
    cdb_key_builder kb;
    cdb_key_init(&kb, k, KEY_MAX);
    cdb_key_u64(&kb, entry);
    return kb.len;
}

static cdb_status get_u32(cdb_index *ix, const uint8_t *k, size_t klen, uint32_t *out, int *found)
{
    uint8_t v[4];
    size_t vlen;
    cdb_status s = cdb_index_get(ix, k, klen, v, sizeof v, &vlen, found);
    if (s != CDB_OK)
        return s;
    if (!*found) {
        *out = 0;
        return CDB_OK;
    }
    if (vlen != 4)
        return CDB_E_CORRUPT;
    *out = cdb_load_u32(v, CDB_ORDER_BIG);
    return CDB_OK;
}

static cdb_status put_u32(cdb_index *ix, const uint8_t *k, size_t klen, uint32_t value)
{
    uint8_t v[4];
    cdb_store_u32(v, value, CDB_ORDER_BIG);
    return cdb_index_put(ix, k, klen, v, sizeof v);
}

static cdb_status get_stats(struct cdb_doc_pool *pool, uint64_t *docs, uint64_t *words)
{
    uint8_t k[KEY_MAX], v[16];
    size_t vlen;
    int found;
    cdb_status s = cdb_index_get(&pool->fts_lengths, k, entry_key(k, 0), v, sizeof v, &vlen, &found);
    *docs = *words = 0;
    if (s != CDB_OK || !found)
        return s;
    if (vlen != 16)
        return CDB_E_CORRUPT;
    *docs = cdb_load_u64(v, CDB_ORDER_BIG);
    *words = cdb_load_u64(v + 8, CDB_ORDER_BIG);
    return CDB_OK;
}

/* Changes a term's document frequency by delta (+1 or -1). */
static cdb_status adjust_df(struct cdb_doc_pool *pool, const cdb_fts_token *t, int delta)
{
    uint8_t k[KEY_MAX];
    size_t klen = term_key(k, t, 0, 0);
    uint32_t df;
    int found;
    cdb_status s = get_u32(&pool->fts_terms, k, klen, &df, &found);
    if (s != CDB_OK)
        return s;
    if (delta < 0) {
        if (df == 0)
            return CDB_E_CORRUPT;
        if (df == 1)
            return cdb_index_delete(&pool->fts_terms, k, klen, &found);
        return put_u32(&pool->fts_terms, k, klen, df - 1u);
    }
    return put_u32(&pool->fts_terms, k, klen, df + 1u);
}

/* -------------------------------------------------------- maintenance */

cdb_status cdb_fts_create(cdb_db *db, struct cdb_doc_pool *pool)
{
    cdb_status s;
    if ((s = cdb_index_create(&pool->fts_terms, &db->pager, pool->slot.store_id,
                              CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX, &db->cfg.catalog_work)) != CDB_OK)
        return s;
    if ((s = cdb_index_create(&pool->fts_postings, &db->pager, pool->slot.store_id,
                              CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX, &db->cfg.catalog_work)) != CDB_OK)
        return s;
    return cdb_index_create(&pool->fts_lengths, &db->pager, pool->slot.store_id, CDB_INDEX_FLAG_UNIQUE,
                            &db->cfg.catalog_work);
}

cdb_status cdb_fts_update(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry,
                          const cdb_fts_doc *before, const cdb_fts_doc *after)
{
    cdb_byte_order o = db->pager.order;
    size_t half = db->cfg.fts_tokens_cap / 2u, na, nb, ta, tb, i = 0, j = 0;
    cdb_fts_token *a = db->cfg.fts_tokens, *b = db->cfg.fts_tokens + half;
    uint8_t k[KEY_MAX];
    uint64_t docs, words;
    int found;
    cdb_status s;

    if (before->body == NULL && after->body == NULL)
        return CDB_OK;
    if ((s = cdb_fts_collect(&pool->fts, before, o, a, half, &na, &ta)) != CDB_OK
        || (s = cdb_fts_collect(&pool->fts, after, o, b, half, &nb, &tb)) != CDB_OK)
        return s;
    /* Walk both sorted word lists: postings and frequencies change only
     * where the two versions differ. */
    while (i < na || j < nb) {
        int c = i == na ? 1 : (j == nb ? -1 : cdb_fts_word_cmp(&a[i], &b[j]));
        if (c < 0) {
            if ((s = cdb_index_delete(&pool->fts_postings, k, term_key(k, &a[i], 1, entry), &found)) != CDB_OK
                || (s = adjust_df(pool, &a[i], -1)) != CDB_OK)
                return s;
            i++;
        } else if (c > 0) {
            if ((s = put_u32(&pool->fts_postings, k, term_key(k, &b[j], 1, entry), b[j].tf)) != CDB_OK
                || (s = adjust_df(pool, &b[j], +1)) != CDB_OK)
                return s;
            j++;
        } else {
            if (a[i].tf != b[j].tf
                && (s = put_u32(&pool->fts_postings, k, term_key(k, &b[j], 1, entry), b[j].tf)) != CDB_OK)
                return s;
            i++;
            j++;
        }
    }
    if (after->body) {
        if ((s = put_u32(&pool->fts_lengths, k, entry_key(k, entry), (uint32_t)tb)) != CDB_OK)
            return s;
    } else if ((s = cdb_index_delete(&pool->fts_lengths, k, entry_key(k, entry), &found)) != CDB_OK) {
        return s;
    }
    if ((s = get_stats(pool, &docs, &words)) != CDB_OK)
        return s;
    if (before->body) {
        docs--;
        words -= ta;
    }
    if (after->body) {
        docs++;
        words += tb;
    }
    {
        uint8_t v[16];
        cdb_store_u64(v, docs, CDB_ORDER_BIG);
        cdb_store_u64(v + 8, words, CDB_ORDER_BIG);
        return cdb_index_put(&pool->fts_lengths, k, entry_key(k, 0), v, sizeof v);
    }
}

/* -------------------------------------------------------- search */

typedef struct term_state {
    uint8_t prefix[KEY_MAX];
    size_t plen;
    double idf;
    uint64_t entry;  /* current posting */
    uint32_t tf;
    int done;
} term_state;

/* Moves a term to its first posting with entry ID >= from. */
static cdb_status advance(cdb_db *db, struct cdb_doc_pool *pool, term_state *t, uint64_t from)
{
    uint8_t k[KEY_MAX], got[KEY_MAX], v[4];
    size_t klen, vlen;
    cdb_index_cursor c;
    int done;
    cdb_status s;
    memcpy(k, t->prefix, t->plen);
    cdb_store_u64(k + t->plen, from, CDB_ORDER_BIG);
    if ((s = cdb_index_seek(&c, &pool->fts_postings, cursor_page(db), k, t->plen + 8u)) != CDB_OK)
        return s;
    if ((s = cdb_index_next(&c, got, sizeof got, &klen, v, sizeof v, &vlen, &done)) != CDB_OK)
        return s;
    if (done || klen != t->plen + 8u || memcmp(got, t->prefix, t->plen) != 0) {
        t->done = 1;
        return CDB_OK;
    }
    if (vlen != 4)
        return CDB_E_CORRUPT;
    t->entry = cdb_load_u64(got + t->plen, CDB_ORDER_BIG);
    t->tf = cdb_load_u32(v, CDB_ORDER_BIG);
    return CDB_OK;
}

/* Keeps hits ordered by score, then by entry ID, at most max of them. */
static void add_hit(cdb_fts_hit *hits, size_t max, size_t *n, uint64_t entry, double score)
{
    size_t pos = *n;
    while (pos > 0 && (hits[pos - 1].score < score || (hits[pos - 1].score == score && hits[pos - 1].entry > entry)))
        pos--;
    if (pos >= max)
        return;
    if (*n < max)
        (*n)++;
    memmove(hits + pos + 1, hits + pos, (*n - pos - 1u) * sizeof *hits);
    hits[pos].entry = entry;
    hits[pos].score = score;
}

static cdb_status score(struct cdb_doc_pool *pool, const term_state *ts, unsigned nt, uint64_t entry,
                        double avgdl, double *out)
{
    uint8_t k[KEY_MAX];
    uint32_t len;
    int found;
    unsigned i;
    double norm;
    cdb_status s = get_u32(&pool->fts_lengths, k, entry_key(k, entry), &len, &found);
    if (s != CDB_OK)
        return s;
    if (!found)
        return CDB_E_CORRUPT; /* a posting for a document the index does not hold */
    norm = BM25_K1 * (1.0 - BM25_B + BM25_B * (double)len / avgdl);
    *out = 0.0;
    for (i = 0; i < nt; i++)
        if (!ts[i].done && ts[i].entry == entry)
            *out += ts[i].idf * ((double)ts[i].tf * (BM25_K1 + 1.0)) / ((double)ts[i].tf + norm);
    return CDB_OK;
}

cdb_status cdb_fts_search(cdb_db *db, struct cdb_doc_pool *pool, const char *query, size_t len, int mode,
                          cdb_fts_hit *hits, size_t max, size_t *n)
{
    term_state ts[CDB_FTS_MAX_QUERY_TERMS];
    cdb_fts_token *words = db->cfg.fts_tokens;
    size_t nw = 0, unique, i;
    unsigned nt = 0;
    uint64_t docs, total;
    double avgdl;
    cdb_status s;

    *n = 0;
    if ((query == NULL && len) || (mode != CDB_FTS_ANY && mode != CDB_FTS_ALL) || (hits == NULL && max))
        return CDB_E_INVALID_ARG;
    if (cdb_fts_split((const uint8_t *)query, len, words, db->cfg.fts_tokens_cap, &nw) > db->cfg.fts_tokens_cap)
        return CDB_E_INVALID_ARG;
    unique = group(words, nw);
    if (unique > CDB_FTS_MAX_QUERY_TERMS)
        return CDB_E_INVALID_ARG;
    if ((s = get_stats(pool, &docs, &total)) != CDB_OK)
        return s;
    if (unique == 0 || docs == 0 || max == 0)
        return CDB_OK;
    avgdl = total ? (double)total / (double)docs : 1.0;

    for (i = 0; i < unique; i++) {
        term_state *t = &ts[nt];
        uint32_t df;
        int found;
        memset(t, 0, sizeof *t);
        t->plen = term_key(t->prefix, &words[i], 0, 0);
        if ((s = get_u32(&pool->fts_terms, t->prefix, t->plen, &df, &found)) != CDB_OK)
            return s;
        if (!found) {
            if (mode == CDB_FTS_ALL)
                return CDB_OK; /* no document has every word */
            continue;
        }
        t->idf = log(1.0 + ((double)docs - (double)df + 0.5) / ((double)df + 0.5));
        if ((s = advance(db, pool, t, 0)) != CDB_OK)
            return s;
        nt++;
    }

    for (;;) {
        uint64_t entry = 0;
        int any = 0, all_same = 1;
        double sc;
        for (i = 0; i < nt; i++) {
            if (ts[i].done) {
                if (mode == CDB_FTS_ALL)
                    return CDB_OK;
                continue;
            }
            if (!any || (mode == CDB_FTS_ANY ? ts[i].entry < entry : ts[i].entry > entry)) {
                if (any)
                    all_same = 0;
                entry = ts[i].entry;
                any = 1;
            } else if (ts[i].entry != entry) {
                all_same = 0;
            }
        }
        if (!any)
            return CDB_OK;
        if (mode == CDB_FTS_ALL && !all_same) {
            /* Leapfrog: every term moves up to the furthest one. */
            for (i = 0; i < nt; i++)
                if (ts[i].entry < entry && (s = advance(db, pool, &ts[i], entry)) != CDB_OK)
                    return s;
            continue;
        }
        if ((s = score(pool, ts, nt, entry, avgdl, &sc)) != CDB_OK)
            return s;
        add_hit(hits, max, n, entry, sc);
        for (i = 0; i < nt; i++)
            if (!ts[i].done && ts[i].entry == entry && (s = advance(db, pool, &ts[i], entry + 1u)) != CDB_OK)
                return s;
    }
}
