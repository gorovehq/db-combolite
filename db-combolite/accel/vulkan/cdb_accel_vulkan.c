/* The Vulkan accelerator (spec 18.5, 18.7-18.10). A mirror holds one row
 * per vector in device memory, packed: row i holds the vector of entry
 * ids[i], and a delete moves the last rows into the holes. Rows and norms
 * are as cax.h describes. A request runs our own compute shaders
 * (shaders/, compiled into cav_spirv.c): sort keys from dot products or
 * bit counts, then the same radix select, gather and bitonic sort as the
 * CUDA kernels. Shaders reach buffers by device address (Vulkan 1.2) and
 * need no optional feature beyond that. Each mirror records into its own
 * command buffer; the one queue is shared under a lock. Its memory comes
 * from malloc and the device: it stands for a device, not for the core. */
#define _POSIX_C_SOURCE 200809L
#include "combolite/cdb_accel_vulkan.h"

#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "cav_spirv.h"
#include "cax.h"
#include "combolite/cdb_value.h"

#define SCRATCH_MAX ((size_t)256u << 20) /* device bytes a request's queries may use at once */
#define CHUNK_MAX 4096u                  /* queries per chunk (a dispatch's y) */
#define GROUP 256u                       /* every shader's workgroup size */
#define SHARED_SORT_MAX 2048u            /* segments up to this many keys sort in shared memory */
#define PUSH_MAX 128u                    /* push constant bytes every device has */
#define RANK_SOFTWARE 3                  /* a device that runs on the CPU: only with CDB_ACCEL_SOFTWARE */

struct cdb_accel_vulkan {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    pthread_mutex_t queue_lock;
    VkPhysicalDeviceMemoryProperties mem;
    VkPipelineLayout layout;
    VkPipeline pipes[CAV_SHADER_COUNT];
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};

/* A buffer, its memory and its device address; mapped when host-visible. */
typedef struct vbuf {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint64_t addr;
    void *map;
} vbuf;

struct cdb_accel_mirror {
    cdb_accel_vulkan *ax;
    cax_shape sh;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    /* Device: rows, per row 1 / |v| or |v|^2, and (Hamming) IDs. */
    vbuf rows, aux, dids;
    size_t n, cap;
    /* Host: the entry ID per row, and entry ID -> row. */
    uint64_t *ids;
    cax_map map;
    /* Per row, the call that last touched it and a row number with it: in a
     * put, where it was staged; in a delete, which original row it holds. */
    uint32_t *stamp, *staged_at, batch;
    double vmax, vmin; /* largest norm ever put, smallest nonzero one */
    uint64_t id_max;
    int tiny; /* a norm too small for its reciprocal to be a float: cosine on the CPU */
    vbuf up, scratch, down; /* host upload, device scratch, host readback */
};

/* ------------------------------------------------------------ errors */

static cdb_status from_vk(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return CDB_OK;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS: return CDB_E_OUT_OF_BUDGET;
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER: return CDB_E_UNSUPPORTED;
    default: return CDB_E_IO;
    }
}

/* ------------------------------------------------------------ buffers */

/* A memory type in `bits` with every flag of `want`, preferring one that
 * also has `prefer`. */
static uint32_t mem_type(const cdb_accel_vulkan *ax, uint32_t bits, VkMemoryPropertyFlags want,
                         VkMemoryPropertyFlags prefer)
{
    uint32_t i, found = UINT32_MAX;
    for (i = 0; i < ax->mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = ax->mem.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want)
            continue;
        if ((f & prefer) == prefer)
            return i;
        if (found == UINT32_MAX)
            found = i;
    }
    return found;
}

static void vb_free(cdb_accel_vulkan *ax, vbuf *b)
{
    if (b->buf != VK_NULL_HANDLE)
        vkDestroyBuffer(ax->dev, b->buf, NULL);
    if (b->mem != VK_NULL_HANDLE)
        vkFreeMemory(ax->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* A buffer of `size` bytes: device-local, or host-visible and mapped. */
static cdb_status vb_make(cdb_accel_vulkan *ax, vbuf *b, VkDeviceSize size, int host)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateFlagsInfo flags;
    VkMemoryAllocateInfo ai;
    VkBufferDeviceAddressInfo da;
    uint32_t type;
    VkResult r;
    memset(b, 0, sizeof *b);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size ? size : 4u;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((r = vkCreateBuffer(ax->dev, &bi, NULL, &b->buf)) != VK_SUCCESS)
        return from_vk(r);
    vkGetBufferMemoryRequirements(ax->dev, b->buf, &req);
    type = host ? mem_type(ax, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                : mem_type(ax, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) {
        vb_free(ax, b);
        return CDB_E_UNSUPPORTED;
    }
    memset(&flags, 0, sizeof flags);
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &flags;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if ((r = vkAllocateMemory(ax->dev, &ai, NULL, &b->mem)) != VK_SUCCESS
        || (r = vkBindBufferMemory(ax->dev, b->buf, b->mem, 0)) != VK_SUCCESS
        || (host && (r = vkMapMemory(ax->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map)) != VK_SUCCESS)) {
        vb_free(ax, b);
        return from_vk(r);
    }
    memset(&da, 0, sizeof da);
    da.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    da.buffer = b->buf;
    b->addr = vkGetBufferDeviceAddress(ax->dev, &da);
    b->size = bi.size;
    return CDB_OK;
}

/* At least `need` bytes, keeping nothing. */
static cdb_status vb_grow(cdb_accel_vulkan *ax, vbuf *b, size_t need, int host)
{
    if (b->buf != VK_NULL_HANDLE && b->size >= need)
        return CDB_OK;
    vb_free(ax, b);
    return vb_make(ax, b, need, host);
}

static size_t up256(size_t n)
{
    return (n + 255u) & ~(size_t)255u;
}

/* ------------------------------------------------------------ commands */

static cdb_status cmd_begin(cdb_accel_mirror *m)
{
    VkCommandBufferBeginInfo bi;
    VkResult r;
    if ((r = vkResetCommandBuffer(m->cmd, 0)) != VK_SUCCESS)
        return from_vk(r);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return from_vk(vkBeginCommandBuffer(m->cmd, &bi));
}

/* Every write so far is visible to every later read, on the device and
 * (for readback) the host. */
static void barrier(cdb_accel_mirror *m)
{
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
                       | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(m->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                             | VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* Ends, submits and waits for the recorded commands. */
static cdb_status cmd_run(cdb_accel_mirror *m)
{
    cdb_accel_vulkan *ax = m->ax;
    VkSubmitInfo si;
    VkResult r;
    if ((r = vkEndCommandBuffer(m->cmd)) != VK_SUCCESS)
        return from_vk(r);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m->cmd;
    pthread_mutex_lock(&ax->queue_lock);
    r = vkQueueSubmit(ax->queue, 1, &si, m->fence);
    pthread_mutex_unlock(&ax->queue_lock);
    if (r != VK_SUCCESS)
        return from_vk(r);
    if ((r = vkWaitForFences(ax->dev, 1, &m->fence, VK_TRUE, UINT64_MAX)) != VK_SUCCESS)
        return from_vk(r);
    return from_vk(vkResetFences(ax->dev, 1, &m->fence));
}

/* Workgroups for n items, `per` items each, at most 1024. */
static uint32_t groups_for(size_t n, size_t per)
{
    size_t g = (n + per - 1u) / per;
    if (g < 1u)
        g = 1u;
    if (g > 1024u)
        g = 1024u;
    return (uint32_t)g;
}

static void dispatch(cdb_accel_mirror *m, unsigned shader, const void *pc, size_t pc_size, uint32_t gx, uint32_t gy)
{
    vkCmdBindPipeline(m->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m->ax->pipes[shader]);
    vkCmdPushConstants(m->cmd, m->ax->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)pc_size, pc);
    vkCmdDispatch(m->cmd, gx, gy, 1);
}

/* Push constants, laid out as the shaders declare them (std430: a uvec2
 * address is a little-endian uint64). */
typedef struct pc_scatter {
    uint64_t dst, src, slots;
    uint32_t words, n;
} pc_scatter;

typedef struct pc_keys {
    uint64_t rows, q, vaux, qaux, keys;
    uint32_t n, words, elem, measure;
} pc_keys;

typedef struct pc_hamming {
    uint64_t rows, q, ids, keys;
    uint32_t n, words, id_bits, pad;
} pc_hamming;

typedef struct pc_sel_init {
    uint64_t state;
    uint32_t nq, want, all_keys, pad;
} pc_sel_init;

typedef struct pc_sel_hist {
    uint64_t keys, state, hist;
    uint32_t n, shift;
} pc_sel_hist;

typedef struct pc_sel_choose {
    uint64_t state, hist;
    uint32_t nq, shift;
} pc_sel_choose;

typedef struct pc_sel_gather {
    uint64_t keys, state, out;
    uint32_t n, seg;
} pc_sel_gather;

typedef struct pc_sort_shared {
    uint64_t out;
    uint32_t seg, pad;
} pc_sort_shared;

typedef struct pc_sort_step {
    uint64_t out;
    uint32_t seg, total, k, jj;
} pc_sort_step;

static void scatter(cdb_accel_mirror *m, uint64_t dst, uint64_t src, uint64_t slots, size_t words, size_t n)
{
    pc_scatter pc;
    pc.dst = dst;
    pc.src = src;
    pc.slots = slots;
    pc.words = (uint32_t)words;
    pc.n = (uint32_t)n;
    dispatch(m, CAV_SCATTER, &pc, sizeof pc, groups_for(n * words, GROUP * 4u), 1);
}

/* ------------------------------------------------------------ growth */

static cdb_status hresize(void **p, size_t size)
{
    void *q = realloc(*p, size);
    if (q == NULL)
        return CDB_E_OUT_OF_BUDGET;
    *p = q;
    return CDB_OK;
}

/* Moves buffer *b to a new one of `size` bytes, keeping its first `keep`. */
static cdb_status dresize(cdb_accel_mirror *m, vbuf *b, size_t keep, size_t size)
{
    vbuf nb;
    cdb_status s;
    if ((s = vb_make(m->ax, &nb, size, 0)) != CDB_OK)
        return s;
    if (keep && b->buf != VK_NULL_HANDLE) {
        VkBufferCopy c;
        c.srcOffset = 0;
        c.dstOffset = 0;
        c.size = keep;
        if ((s = cmd_begin(m)) == CDB_OK) {
            vkCmdCopyBuffer(m->cmd, b->buf, nb.buf, 1, &c);
            s = cmd_run(m);
        }
        if (s != CDB_OK) {
            vb_free(m->ax, &nb);
            return s;
        }
    }
    vb_free(m->ax, b);
    *b = nb;
    return CDB_OK;
}

/* Room for at least `need` vectors: device rows and host tables grow to
 * twice what is needed. Shaders index rows in 32 bits. */
static cdb_status reserve(cdb_accel_mirror *m, size_t need)
{
    size_t cap, row = m->sh.words * 4u;
    cdb_status s;
    if (need <= m->cap)
        return CDB_OK;
    cap = m->cap * 2u > need ? m->cap * 2u : need;
    if (cap < 64u)
        cap = 64u;
    if ((uint64_t)cap * m->sh.words >= UINT32_MAX)
        return CDB_E_OUT_OF_BUDGET;
    if ((s = dresize(m, &m->rows, m->n * row, cap * row)) != CDB_OK
        || (s = dresize(m, &m->aux, m->n * 4u, cap * 4u)) != CDB_OK
        || (m->sh.measure == CAX_HAMMING && (s = dresize(m, &m->dids, m->n * 8u, cap * 8u)) != CDB_OK)
        || (s = hresize((void **)&m->ids, cap * 8u)) != CDB_OK
        || (s = hresize((void **)&m->stamp, cap * 4u)) != CDB_OK
        || (s = hresize((void **)&m->staged_at, cap * 4u)) != CDB_OK)
        return s;
    memset(m->stamp + m->cap, 0, (cap - m->cap) * 4u);
    m->cap = cap;
    return cax_map_reserve(&m->map, cap, m->ids, m->n);
}

/* A new call's stamp, distinct from every stamp in use. */
static uint32_t next_batch(cdb_accel_mirror *m)
{
    if (++m->batch == 0) {
        memset(m->stamp, 0, m->cap * 4u);
        m->batch = 1;
    }
    return m->batch;
}

/* ------------------------------------------------------------ calls */

static void free_mirror(cdb_accel_mirror *m)
{
    cdb_accel_vulkan *ax = m->ax;
    vb_free(ax, &m->rows);
    vb_free(ax, &m->aux);
    vb_free(ax, &m->dids);
    vb_free(ax, &m->up);
    vb_free(ax, &m->scratch);
    vb_free(ax, &m->down);
    if (m->fence != VK_NULL_HANDLE)
        vkDestroyFence(ax->dev, m->fence, NULL);
    if (m->pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(ax->dev, m->pool, NULL);
    free(m->ids);
    cax_map_free(&m->map);
    free(m->stamp);
    free(m->staged_at);
    free(m);
}

static cdb_status v_open(void *ctx, uint64_t store, unsigned elem, uint32_t dims, unsigned measure,
                         cdb_byte_order order, uint64_t expect, cdb_accel_mirror **out)
{
    cdb_accel_vulkan *ax = (cdb_accel_vulkan *)ctx;
    cdb_accel_mirror *m;
    VkCommandPoolCreateInfo pi;
    VkCommandBufferAllocateInfo ci;
    VkFenceCreateInfo fi;
    cax_shape sh;
    cdb_status s;
    (void)store;
    if ((s = cax_shape_init(&sh, elem, dims, measure, order)) != CDB_OK)
        return s;
    if ((m = (cdb_accel_mirror *)calloc(1, sizeof *m)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    m->ax = ax;
    m->sh = sh;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = ax->family;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if ((s = from_vk(vkCreateCommandPool(ax->dev, &pi, NULL, &m->pool))) == CDB_OK) {
        ci.commandPool = m->pool;
        if ((s = from_vk(vkAllocateCommandBuffers(ax->dev, &ci, &m->cmd))) == CDB_OK
            && (s = from_vk(vkCreateFence(ax->dev, &fi, NULL, &m->fence))) == CDB_OK)
            s = reserve(m, expect < (uint64_t)(UINT32_MAX / 2u) ? (size_t)expect : 64u);
    }
    if (s != CDB_OK) {
        free_mirror(m);
        return s;
    }
    *out = m;
    return CDB_OK;
}

static cdb_status v_put(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, const void *vecs, size_t vec_bytes,
                        size_t n)
{
    size_t i, staged = 0, row = m->sh.words * 4u, need, o_aux, o_slots, o_ids;
    uint8_t *h;
    uint32_t *hrows, *hslots, batch;
    float *haux;
    uint64_t *hids;
    cdb_status s;
    (void)ctx;
    if (vec_bytes != m->sh.vec_bytes)
        return CDB_E_INVALID_ARG;
    if (n == 0)
        return CDB_OK;
    if ((s = reserve(m, m->n + n)) != CDB_OK)
        return s;
    o_aux = up256(n * row);
    o_slots = o_aux + up256(n * 4u);
    o_ids = o_slots + up256(n * 4u);
    need = o_ids + up256(n * 8u);
    if ((s = vb_grow(m->ax, &m->up, need, 1)) != CDB_OK)
        return s;
    h = (uint8_t *)m->up.map;
    hrows = (uint32_t *)h;
    haux = (float *)(h + o_aux);
    hslots = (uint32_t *)(h + o_slots);
    hids = (uint64_t *)(h + o_ids);
    batch = next_batch(m);
    for (i = 0; i < n; i++) {
        size_t at = cax_map_find(&m->map, ids[i]), r;
        uint32_t slot = m->map.rows[at];
        double sum, norm;
        if (slot == CAX_NO_ROW) {
            slot = (uint32_t)m->n++;
            m->ids[slot] = ids[i];
            m->map.ids[at] = ids[i];
            m->map.rows[at] = slot;
            if (ids[i] > m->id_max)
                m->id_max = ids[i];
        }
        if (m->stamp[slot] == batch) {
            r = m->staged_at[slot];
        } else {
            r = staged++;
            m->stamp[slot] = batch;
            m->staged_at[slot] = (uint32_t)r;
            hslots[r] = slot;
        }
        sum = cax_to_row(&m->sh, (const uint8_t *)vecs + i * vec_bytes, hrows + r * m->sh.words);
        norm = sqrt(sum);
        if (!(norm <= CAX_NORM_MAX))
            return CDB_E_UNSUPPORTED; /* the mirror goes stale; the CPU answers */
        if (m->sh.measure == CAX_EUCLIDEAN) {
            haux[r] = (float)sum;
        } else if (m->sh.measure == CAX_COSINE) {
            float vn = (float)norm; /* the CPU path's rounded norm */
            double rv = vn == 0.0f ? 0.0 : 1.0 / (double)vn;
            if (rv > FLT_MAX) {
                m->tiny = 1;
                rv = FLT_MAX;
            }
            haux[r] = (float)rv;
        } else {
            haux[r] = 0.0f;
        }
        hids[r] = ids[i];
        if (norm > m->vmax)
            m->vmax = norm;
        if (norm > 0.0 && (m->vmin == 0.0 || norm < m->vmin))
            m->vmin = norm;
    }
    if ((s = cmd_begin(m)) != CDB_OK)
        return s;
    scatter(m, m->rows.addr, m->up.addr, m->up.addr + o_slots, m->sh.words, staged);
    scatter(m, m->aux.addr, m->up.addr + o_aux, m->up.addr + o_slots, 1u, staged);
    if (m->sh.measure == CAX_HAMMING)
        scatter(m, m->dids.addr, m->up.addr + o_ids, m->up.addr + o_slots, 2u, staged);
    return cmd_run(m);
}

/* Deletes as the CUDA backend does, the last row into each hole, but on
 * the host first: each row's stamp then says which original row it holds.
 * The rows moved come from at or past the final count and go below it, so
 * one copy with every region moves them all. */
static cdb_status v_del(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, size_t n)
{
    size_t i, moves = 0, row = m->sh.words * 4u;
    uint32_t *touched, batch;
    VkBufferCopy *rc, *ac, *ic;
    cdb_status s = CDB_OK;
    (void)ctx;
    if (n == 0)
        return CDB_OK;
    touched = (uint32_t *)malloc(n * sizeof *touched);
    rc = (VkBufferCopy *)malloc(n * sizeof *rc);
    ac = (VkBufferCopy *)malloc(n * sizeof *ac);
    ic = (VkBufferCopy *)malloc(n * sizeof *ic);
    if (touched == NULL || rc == NULL || ac == NULL || ic == NULL) {
        s = CDB_E_OUT_OF_BUDGET;
        goto out;
    }
    batch = next_batch(m);
    for (i = 0; i < n; i++) {
        size_t at = cax_map_find(&m->map, ids[i]), last;
        uint32_t slot = m->map.rows[at];
        if (slot == CAX_NO_ROW)
            continue;
        cax_map_erase(&m->map, at);
        last = --m->n;
        if (slot != last) {
            uint64_t moved = m->ids[last];
            uint32_t origin = m->stamp[last] == batch ? m->staged_at[last] : (uint32_t)last;
            m->ids[slot] = moved;
            m->map.rows[cax_map_find(&m->map, moved)] = slot;
            if (m->stamp[slot] != batch)
                touched[moves++] = slot;
            m->stamp[slot] = batch;
            m->staged_at[slot] = origin;
        }
    }
    {
        size_t k = 0;
        for (i = 0; i < moves; i++) {
            uint32_t slot = touched[i], from;
            if (slot >= m->n || m->stamp[slot] != batch)
                continue; /* moved into, then moved out again */
            from = m->staged_at[slot];
            rc[k].srcOffset = (VkDeviceSize)from * row;
            rc[k].dstOffset = (VkDeviceSize)slot * row;
            rc[k].size = row;
            ac[k].srcOffset = (VkDeviceSize)from * 4u;
            ac[k].dstOffset = (VkDeviceSize)slot * 4u;
            ac[k].size = 4u;
            ic[k].srcOffset = (VkDeviceSize)from * 8u;
            ic[k].dstOffset = (VkDeviceSize)slot * 8u;
            ic[k].size = 8u;
            k++;
        }
        if (k > 0 && (s = cmd_begin(m)) == CDB_OK) {
            vkCmdCopyBuffer(m->cmd, m->rows.buf, m->rows.buf, (uint32_t)k, rc);
            vkCmdCopyBuffer(m->cmd, m->aux.buf, m->aux.buf, (uint32_t)k, ac);
            if (m->sh.measure == CAX_HAMMING)
                vkCmdCopyBuffer(m->cmd, m->dids.buf, m->dids.buf, (uint32_t)k, ic);
            s = cmd_run(m);
        }
    }
out:
    free(touched);
    free(rc);
    free(ac);
    free(ic);
    return s;
}

static void v_close(void *ctx, cdb_accel_mirror *m)
{
    (void)ctx;
    free_mirror(m);
}

static cdb_status v_nearest(void *ctx, cdb_accel_mirror *m, const void *queries, size_t nq, size_t want,
                            uint64_t *ids, float *dists, size_t *counts, double *bounds)
{
    const cax_shape *sh = &m->sh;
    size_t w, seg, per, chunk, q0, row = sh->words * 4u;
    unsigned id_bits = 0;
    int hamming = sh->measure == CAX_HAMMING;
    cdb_status s;
    (void)ctx;
    if (m->n == 0 || want == 0) {
        for (q0 = 0; q0 < nq; q0++) {
            counts[q0] = 0;
            bounds[q0] = 0.0;
        }
        return CDB_OK;
    }
    if (hamming) {
        id_bits = 64u - cax_bits_for(sh->dims);
        if (m->id_max >> id_bits)
            return CDB_E_UNSUPPORTED; /* IDs too large to order ties by on the device */
    }
    if (m->tiny && sh->measure == CAX_COSINE)
        return CDB_E_UNSUPPORTED;
    w = want < m->n ? want : m->n;
    for (seg = 1; seg < w; seg *= 2u)
        ;
    per = up256(row) + 4u + m->n * 8u + seg * 8u + 32u + 1024u + 1024u;
    chunk = SCRATCH_MAX / per;
    if (chunk > (size_t)(UINT32_MAX / 2u) / (m->n > seg ? m->n : seg))
        chunk = (size_t)(UINT32_MAX / 2u) / (m->n > seg ? m->n : seg); /* key indices are 32-bit */
    if (chunk < 1u)
        chunk = 1u;
    if (chunk > CHUNK_MAX)
        chunk = CHUNK_MAX;
    if (chunk > nq)
        chunk = nq;
    for (q0 = 0; q0 < nq; q0 += chunk) {
        size_t qc = nq - q0 < chunk ? nq - q0 : chunk, j, r, pass;
        size_t o_q = 0, o_qaux = up256(qc * row), o_keys = o_qaux + up256(qc * 4u);
        size_t o_out = o_keys + up256(m->n * qc * 8u), o_state = o_out + up256(qc * seg * 8u);
        size_t o_hist = o_state + up256(qc * 32u), dneed = o_hist + up256(qc * 1024u);
        uint64_t d;
        uint8_t *h;
        float *qaux;
        double qn[CHUNK_MAX];
        VkBufferCopy cp[2];
        VkBufferCopy *back;
        if ((s = vb_grow(m->ax, &m->scratch, dneed, 0)) != CDB_OK
            || (s = vb_grow(m->ax, &m->up, o_keys, 1)) != CDB_OK
            || (s = vb_grow(m->ax, &m->down, qc * w * 8u, 1)) != CDB_OK)
            return s;
        d = m->scratch.addr;
        h = (uint8_t *)m->up.map;
        qaux = (float *)(h + o_qaux);
        for (j = 0; j < qc; j++) {
            double sum = cax_to_row(sh, (const uint8_t *)queries + (q0 + j) * sh->vec_bytes, (uint32_t *)(h + j * row));
            double norm = sqrt(sum);
            if (!(norm <= CAX_NORM_MAX) || (sh->measure == CAX_COSINE && (norm == 0.0 || 1.0 / norm > FLT_MAX)))
                return CDB_E_UNSUPPORTED; /* the CPU answers: overflow, or every distance ties at 1 */
            qn[j] = norm;
            qaux[j] = sh->measure == CAX_EUCLIDEAN ? (float)sum : sh->measure == CAX_COSINE ? (float)(1.0 / norm) : 0.0f;
        }
        if ((back = (VkBufferCopy *)malloc(qc * sizeof *back)) == NULL)
            return CDB_E_OUT_OF_BUDGET;
        if ((s = cmd_begin(m)) != CDB_OK) {
            free(back);
            return s;
        }
        cp[0].srcOffset = 0;
        cp[0].dstOffset = o_q;
        cp[0].size = qc * row;
        cp[1].srcOffset = o_qaux;
        cp[1].dstOffset = o_qaux;
        cp[1].size = qc * 4u;
        vkCmdCopyBuffer(m->cmd, m->up.buf, m->scratch.buf, 2, cp);
        barrier(m);
        if (hamming) {
            pc_hamming pc;
            pc.rows = m->rows.addr;
            pc.q = d + o_q;
            pc.ids = m->dids.addr;
            pc.keys = d + o_keys;
            pc.n = (uint32_t)m->n;
            pc.words = (uint32_t)sh->words;
            pc.id_bits = id_bits;
            pc.pad = 0;
            dispatch(m, CAV_HAMMING, &pc, sizeof pc, groups_for(m->n, GROUP * 4u), (uint32_t)qc);
        } else {
            pc_keys pc;
            pc.rows = m->rows.addr;
            pc.q = d + o_q;
            pc.vaux = m->aux.addr;
            pc.qaux = d + o_qaux;
            pc.keys = d + o_keys;
            pc.n = (uint32_t)m->n;
            pc.words = (uint32_t)sh->words;
            pc.elem = sh->elem;
            pc.measure = sh->measure;
            dispatch(m, CAV_KEYS, &pc, sizeof pc, groups_for(m->n, GROUP / 32u), (uint32_t)qc); /* 32 lanes a row */
        }
        barrier(m);
        {
            pc_sel_init pc;
            pc.state = d + o_state;
            pc.nq = (uint32_t)qc;
            pc.want = (uint32_t)w;
            pc.all_keys = w >= m->n ? 1u : 0u;
            pc.pad = 0;
            dispatch(m, CAV_SEL_INIT, &pc, sizeof pc, groups_for(qc, GROUP), 1);
        }
        barrier(m);
        if (w < m->n)
            for (pass = 0; pass < 8u; pass++) {
                pc_sel_hist ph;
                pc_sel_choose pcs;
                uint32_t shift = (uint32_t)(56u - 8u * pass);
                vkCmdFillBuffer(m->cmd, m->scratch.buf, o_hist, qc * 1024u, 0);
                barrier(m);
                ph.keys = d + o_keys;
                ph.state = d + o_state;
                ph.hist = d + o_hist;
                ph.n = (uint32_t)m->n;
                ph.shift = shift;
                dispatch(m, CAV_SEL_HIST, &ph, sizeof ph, groups_for(m->n, GROUP * 8u), (uint32_t)qc);
                barrier(m);
                pcs.state = d + o_state;
                pcs.hist = d + o_hist;
                pcs.nq = (uint32_t)qc;
                pcs.shift = shift;
                dispatch(m, CAV_SEL_CHOOSE, &pcs, sizeof pcs, groups_for(qc, GROUP), 1);
                barrier(m);
            }
        vkCmdFillBuffer(m->cmd, m->scratch.buf, o_out, qc * seg * 8u, 0xFFFFFFFFu);
        barrier(m);
        {
            pc_sel_gather pc;
            pc.keys = d + o_keys;
            pc.state = d + o_state;
            pc.out = d + o_out;
            pc.n = (uint32_t)m->n;
            pc.seg = (uint32_t)seg;
            dispatch(m, CAV_SEL_GATHER, &pc, sizeof pc, groups_for(m->n, GROUP * 8u), (uint32_t)qc);
        }
        barrier(m);
        if (seg <= SHARED_SORT_MAX) {
            pc_sort_shared pc;
            pc.out = d + o_out;
            pc.seg = (uint32_t)seg;
            pc.pad = 0;
            dispatch(m, CAV_SORT_SHARED, &pc, sizeof pc, (uint32_t)qc, 1);
        } else {
            size_t k, jj, total = qc * seg;
            for (k = 2; k <= seg; k <<= 1)
                for (jj = k >> 1; jj > 0; jj >>= 1) {
                    pc_sort_step pc;
                    pc.out = d + o_out;
                    pc.seg = (uint32_t)seg;
                    pc.total = (uint32_t)total;
                    pc.k = (uint32_t)k;
                    pc.jj = (uint32_t)jj;
                    dispatch(m, CAV_SORT_STEP, &pc, sizeof pc, groups_for(total, GROUP * 4u), 1);
                    barrier(m);
                }
        }
        barrier(m);
        for (j = 0; j < qc; j++) {
            back[j].srcOffset = o_out + j * seg * 8u;
            back[j].dstOffset = j * w * 8u;
            back[j].size = w * 8u;
        }
        vkCmdCopyBuffer(m->cmd, m->scratch.buf, m->down.buf, (uint32_t)qc, back);
        barrier(m);
        s = cmd_run(m);
        free(back);
        if (s != CDB_OK)
            return s;
        for (j = 0; j < qc; j++) {
            const uint8_t *keys = (const uint8_t *)m->down.map + j * w * 8u;
            uint64_t *oi = ids + (q0 + j) * want;
            float *od = dists + (q0 + j) * want;
            for (r = 0; r < w; r++) {
                uint64_t key;
                memcpy(&key, keys + r * 8u, 8u); /* little-endian, as the device wrote it */
                if (hamming) {
                    od[r] = (float)(key >> id_bits);
                    oi[r] = key & ((UINT64_C(1) << id_bits) - 1u);
                } else {
                    od[r] = cax_from_order((uint32_t)(key >> 32));
                    oi[r] = m->ids[(uint32_t)key];
                }
            }
            counts[q0 + j] = w;
            bounds[q0 + j] = cax_bound(sh, CAX_EPILOGUE_FLOAT, qn[j], m->vmax, m->vmin);
        }
    }
    return CDB_OK;
}

static const cdb_accel_ops vulkan_ops = { v_open, v_put, v_del, v_close, v_nearest };

/* ------------------------------------------------------------ devices */

static cdb_status make_instance(VkInstance *inst)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "combolite_accel";
    app.apiVersion = VK_API_VERSION_1_2;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    return vkCreateInstance(&ci, NULL, inst) == VK_SUCCESS ? CDB_OK : CDB_E_UNSUPPORTED;
}

/* Whether a device can run the shaders, and its rank (lower is better);
 * *family is its first compute queue family. */
static int usable(VkPhysicalDevice p, uint32_t *family, int *rank)
{
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceFeatures2 f2;
    VkQueueFamilyProperties qf[32];
    uint32_t nqf = 32, i;
    vkGetPhysicalDeviceProperties(p, &props);
    if (props.apiVersion < VK_API_VERSION_1_2)
        return 0;
    memset(&f12, 0, sizeof f12);
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    memset(&f2, 0, sizeof f2);
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(p, &f2);
    if (!f12.bufferDeviceAddress || props.limits.maxComputeWorkGroupInvocations < GROUP
        || props.limits.maxComputeWorkGroupSize[0] < GROUP || props.limits.maxComputeSharedMemorySize < 16384u
        || props.limits.maxComputeWorkGroupCount[1] < CHUNK_MAX || props.limits.maxPushConstantsSize < PUSH_MAX)
        return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &nqf, qf);
    for (i = 0; i < nqf; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            break;
    if (i == nqf)
        return 0;
    *family = i;
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: *rank = 0; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: *rank = 1; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: *rank = 2; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: *rank = RANK_SOFTWARE; break;
    default: *rank = 4; break;
    }
    return 1;
}

int cdb_accel_vulkan_devices(void)
{
    VkInstance inst;
    uint32_t n = 0;
    if (make_instance(&inst) != CDB_OK)
        return 0;
    if (vkEnumeratePhysicalDevices(inst, &n, NULL) != VK_SUCCESS)
        n = 0;
    vkDestroyInstance(inst, NULL);
    return (int)n;
}

void cdb_accel_vulkan_close(cdb_accel_vulkan *ax)
{
    unsigned i;
    if (ax->dev != VK_NULL_HANDLE) {
        for (i = 0; i < CAV_SHADER_COUNT; i++)
            if (ax->pipes[i] != VK_NULL_HANDLE)
                vkDestroyPipeline(ax->dev, ax->pipes[i], NULL);
        if (ax->layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(ax->dev, ax->layout, NULL);
        vkDestroyDevice(ax->dev, NULL);
    }
    if (ax->inst != VK_NULL_HANDLE)
        vkDestroyInstance(ax->inst, NULL);
    pthread_mutex_destroy(&ax->queue_lock);
    free(ax);
}

static cdb_status make_pipelines(cdb_accel_vulkan *ax)
{
    VkPushConstantRange range;
    VkPipelineLayoutCreateInfo li;
    unsigned i;
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = PUSH_MAX;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ax->dev, &li, NULL, &ax->layout) != VK_SUCCESS)
        return CDB_E_UNSUPPORTED;
    for (i = 0; i < CAV_SHADER_COUNT; i++) {
        VkShaderModuleCreateInfo mi;
        VkComputePipelineCreateInfo pi;
        VkShaderModule mod;
        VkResult r;
        memset(&mi, 0, sizeof mi);
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = cav_shaders[i].size;
        mi.pCode = cav_shaders[i].code;
        if (vkCreateShaderModule(ax->dev, &mi, NULL, &mod) != VK_SUCCESS)
            return CDB_E_UNSUPPORTED;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = mod;
        pi.stage.pName = "main";
        pi.layout = ax->layout;
        r = vkCreateComputePipelines(ax->dev, VK_NULL_HANDLE, 1, &pi, NULL, &ax->pipes[i]);
        vkDestroyShaderModule(ax->dev, mod, NULL);
        if (r != VK_SUCCESS)
            return CDB_E_UNSUPPORTED;
    }
    return CDB_OK;
}

cdb_status cdb_accel_vulkan_open(int device, unsigned flags, cdb_accel_vulkan **out)
{
    cdb_accel_vulkan *ax;
    VkPhysicalDevice devs[16];
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceVulkan12Features f12;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    uint32_t n = 16, i, family = 0;
    int best = -1, best_rank = 99;
    float prio = 1.0f;
    cdb_status s;
    if ((ax = (cdb_accel_vulkan *)calloc(1, sizeof *ax)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    pthread_mutex_init(&ax->queue_lock, NULL);
    if (make_instance(&ax->inst) != CDB_OK) {
        cdb_accel_vulkan_close(ax);
        return CDB_E_UNSUPPORTED;
    }
    if (vkEnumeratePhysicalDevices(ax->inst, &n, devs) < 0)
        n = 0;
    for (i = 0; i < n; i++) {
        uint32_t fam;
        int rank;
        if ((device < 0 || (uint32_t)device == i) && usable(devs[i], &fam, &rank)
            && (rank != RANK_SOFTWARE || (flags & CDB_ACCEL_SOFTWARE)) && rank < best_rank) {
            best = (int)i;
            best_rank = rank;
            family = fam;
        }
    }
    if (best < 0) {
        cdb_accel_vulkan_close(ax);
        return CDB_E_UNSUPPORTED;
    }
    ax->phys = devs[best];
    ax->family = family;
    vkGetPhysicalDeviceProperties(ax->phys, &props);
    vkGetPhysicalDeviceMemoryProperties(ax->phys, &ax->mem);
    snprintf(ax->name, sizeof ax->name, "%s", props.deviceName);
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    memset(&f12, 0, sizeof f12);
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.bufferDeviceAddress = VK_TRUE;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext = &f12;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    if (vkCreateDevice(ax->phys, &di, NULL, &ax->dev) != VK_SUCCESS) {
        ax->dev = VK_NULL_HANDLE;
        cdb_accel_vulkan_close(ax);
        return CDB_E_UNSUPPORTED;
    }
    vkGetDeviceQueue(ax->dev, family, 0, &ax->queue);
    if ((s = make_pipelines(ax)) != CDB_OK) {
        cdb_accel_vulkan_close(ax);
        return s;
    }
    *out = ax;
    return CDB_OK;
}

void cdb_accel_vulkan_attach(cdb_accel_vulkan *ax, cdb_env *env)
{
    env->accel = &vulkan_ops;
    env->accel_ctx = ax;
}

const char *cdb_accel_vulkan_name(const cdb_accel_vulkan *ax)
{
    return ax->name;
}
