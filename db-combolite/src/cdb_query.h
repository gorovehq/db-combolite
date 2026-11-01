/* Structured query core (CMBQL core): binary wire format and operator tree
 * (spec 11.1, 11.7, 11.10, 14.19).
 *
 * Every query has a canonical binary wire representation. The engine evaluates
 * queries incrementally through streaming cursors under strict memory budgets.
 *
 * The operator tree is read-only (spec 11.10): mutations (insert, replace,
 * delete, link) are executed via stored procedures or direct engine APIs,
 * never as plan nodes. */
#ifndef CDB_QUERY_H
#define CDB_QUERY_H

#include "combolite/cdb_common.h"
#include "combolite/cdb_value.h"

#define CDB_QUERY_MAGIC_0 0x43u /* 'C' */
#define CDB_QUERY_MAGIC_1 0x4Du /* 'M' */
#define CDB_QUERY_MAGIC_2 0x42u /* 'B' */
#define CDB_QUERY_MAGIC_3 0x51u /* 'Q' */
#define CDB_QUERY_VERSION_1 1u
#define CDB_QUERY_HEADER_SIZE 6u
#define CDB_QUERY_MAX_DEPTH 64u

/* Nested field delimiter: ASCII Unit Separator (0x1F, spec 11.10). */
#define CDB_QUERY_PATH_SEP '\x1F'

/* Operator node tags (spec 14.19.2). */
#define CDB_QUERY_NODE_SCAN 0x01u
#define CDB_QUERY_NODE_FILTER 0x02u
#define CDB_QUERY_NODE_PROJECT 0x03u
#define CDB_QUERY_NODE_JOIN 0x04u
#define CDB_QUERY_NODE_SORT 0x05u
#define CDB_QUERY_NODE_LIMIT 0x06u
#define CDB_QUERY_NODE_UNION 0x07u
#define CDB_QUERY_NODE_AGGREGATE 0x08u
#define CDB_QUERY_NODE_TRAVERSE 0x09u
#define CDB_QUERY_NODE_TEXT_SEARCH 0x0Au
#define CDB_QUERY_NODE_VECTOR_SEARCH 0x0Bu
#define CDB_QUERY_NODE_HYBRID_RANK 0x0Cu
#define CDB_QUERY_NODE_WINDOW 0x0Du
#define CDB_QUERY_NODE_ASOF_JOIN 0x0Eu
#define CDB_QUERY_NODE_UNNEST 0x0Fu

/* Expression tags (spec 14.19.3). */
#define CDB_QUERY_EXPR_LITERAL 0x20u
#define CDB_QUERY_EXPR_FIELD 0x21u
#define CDB_QUERY_EXPR_PARAM 0x22u
#define CDB_QUERY_EXPR_UNARY 0x23u
#define CDB_QUERY_EXPR_BINARY 0x24u
#define CDB_QUERY_EXPR_CALL 0x25u

/* Join kinds. */
#define CDB_JOIN_INNER 0x01u
#define CDB_JOIN_LEFT_OUTER 0x02u

/* Sort direction & nulls placement. */
#define CDB_SORT_ASC 0x01u
#define CDB_SORT_DESC 0x02u
#define CDB_NULLS_FIRST 0x01u
#define CDB_NULLS_LAST 0x02u

/* Traverse direction. */
#define CDB_TRAVERSE_FWD 0x01u
#define CDB_TRAVERSE_REV 0x02u
#define CDB_TRAVERSE_BOTH 0x03u

/* Text search mode. */
#define CDB_QUERY_FTS_ANY 0x01u
#define CDB_QUERY_FTS_ALL 0x02u

/* Vector search metric override. */
#define CDB_QUERY_METRIC_DEFAULT 0x00u
#define CDB_QUERY_METRIC_EUCLIDEAN 0x01u
#define CDB_QUERY_METRIC_COSINE 0x02u
#define CDB_QUERY_METRIC_DOT 0x03u

/* Hybrid rank fusion method. */
#define CDB_FUSE_RRF 0x01u

/* Time-series window type. */
#define CDB_WINDOW_TUMBLING 0x01u
#define CDB_WINDOW_SLIDING 0x02u
#define CDB_WINDOW_HOPPING 0x03u

/* AsofJoin kind. */
#define CDB_ASOF_BACKWARD 0x01u
#define CDB_ASOF_FORWARD 0x02u
#define CDB_ASOF_NEAREST 0x03u

/* Parameter kind. */
#define CDB_PARAM_INDEX 0x01u
#define CDB_PARAM_NAMED 0x02u

/* Unary operators. */
#define CDB_UNARY_NOT 0x01u
#define CDB_UNARY_NEG 0x02u
#define CDB_UNARY_IS_NULL 0x03u
#define CDB_UNARY_IS_NOT_NULL 0x04u

/* Binary operators. */
#define CDB_BIN_AND 0x01u
#define CDB_BIN_OR 0x02u
#define CDB_BIN_EQ 0x03u
#define CDB_BIN_NE 0x04u
#define CDB_BIN_LT 0x05u
#define CDB_BIN_LE 0x06u
#define CDB_BIN_GT 0x07u
#define CDB_BIN_GE 0x08u
#define CDB_BIN_ADD 0x09u
#define CDB_BIN_SUB 0x0Au
#define CDB_BIN_MUL 0x0Bu
#define CDB_BIN_DIV 0x0Cu
#define CDB_BIN_MOD 0x0Du
#define CDB_BIN_LIKE 0x0Eu
#define CDB_BIN_CONTAINS 0x0Fu

/* Aggregate functions. */
#define CDB_AGG_COUNT 0x01u
#define CDB_AGG_SUM 0x02u
#define CDB_AGG_AVG 0x03u
#define CDB_AGG_MIN 0x04u
#define CDB_AGG_MAX 0x05u
#define CDB_AGG_FIRST 0x06u
#define CDB_AGG_LAST 0x07u
#define CDB_AGG_STDDEV 0x08u
#define CDB_AGG_VARIANCE 0x09u

/* Math, Geometry, DSP and Linear Algebra function IDs (CDB_QUERY_EXPR_CALL). */
/* Trigonometry & Hyperbolics */
#define CDB_FN_SIN 0x0100u
#define CDB_FN_COS 0x0101u
#define CDB_FN_TAN 0x0102u
#define CDB_FN_ASIN 0x0103u
#define CDB_FN_ACOS 0x0104u
#define CDB_FN_ATAN 0x0105u
#define CDB_FN_ATAN2 0x0106u
#define CDB_FN_SINH 0x0107u
#define CDB_FN_COSH 0x0108u
#define CDB_FN_TANH 0x0109u
#define CDB_FN_DEGREES 0x010Au
#define CDB_FN_RADIANS 0x010Bu

/* Geometry & Spatial / Geodesic / Navigation & GIS Projections */
#define CDB_FN_HYPOT 0x0200u
#define CDB_FN_HYPOT3 0x0201u
#define CDB_FN_HAVERSINE 0x0202u
#define CDB_FN_BBOX_CONTAINS 0x0203u
#define CDB_FN_BEARING 0x0204u
#define CDB_FN_DESTINATION_LAT 0x0205u
#define CDB_FN_DESTINATION_LON 0x0206u
#define CDB_FN_CROSS_TRACK_DISTANCE 0x0207u
#define CDB_FN_ALONG_TRACK_DISTANCE 0x0208u
#define CDB_FN_POINT_IN_CIRCLE 0x0209u
#define CDB_FN_POINT_IN_POLYGON 0x020Au
#define CDB_FN_WGS84_TO_MERCATOR_X 0x020Bu
#define CDB_FN_WGS84_TO_MERCATOR_Y 0x020Cu
#define CDB_FN_MERCATOR_TO_WGS84_LAT 0x020Du
#define CDB_FN_MERCATOR_TO_WGS84_LON 0x020Eu
#define CDB_FN_TILE_X 0x020Fu
#define CDB_FN_TILE_Y 0x0210u
#define CDB_FN_MORTON2D 0x0211u
#define CDB_FN_GEOHASH_ENCODE 0x0212u
#define CDB_FN_GEOHASH_DECODE_LAT 0x0213u
#define CDB_FN_GEOHASH_DECODE_LON 0x0214u

/* Algebra, Roots, Powers, Logarithms & ML Activations */
#define CDB_FN_SQRT 0x0300u
#define CDB_FN_CBRT 0x0301u
#define CDB_FN_POW 0x0302u
#define CDB_FN_EXP 0x0303u
#define CDB_FN_EXPM1 0x0304u
#define CDB_FN_LN 0x0305u
#define CDB_FN_LOG10 0x0306u
#define CDB_FN_LOG2 0x0307u
#define CDB_FN_LOG1P 0x0308u
#define CDB_FN_ABS 0x0309u
#define CDB_FN_SIGN 0x030Au
#define CDB_FN_ROUND 0x030Bu
#define CDB_FN_FLOOR 0x030Cu
#define CDB_FN_CEIL 0x030Du
#define CDB_FN_CLAMP 0x030Eu
#define CDB_FN_LERP 0x030Fu
#define CDB_FN_SIGMOID 0x0310u
#define CDB_FN_RELU 0x0311u
#define CDB_FN_GELU 0x0312u
#define CDB_FN_ERF 0x0313u

/* Calculus & Numerical Analysis */
#define CDB_FN_DIFF 0x0400u
#define CDB_FN_DERIVATIVE 0x0401u
#define CDB_FN_TRAPEZOID 0x0402u

/* Linear Algebra: Vectors & Matrices */
#define CDB_FN_VEC_DOT 0x0500u
#define CDB_FN_VEC_NORM 0x0501u
#define CDB_FN_VEC_COSINE 0x0502u
#define CDB_FN_VEC_DISTANCE 0x0503u
#define CDB_FN_VEC_SCALE 0x0504u
#define CDB_FN_VEC_ADD 0x0505u
#define CDB_FN_VEC_SUB 0x0506u
#define CDB_FN_MAT_VEC_MUL 0x0510u
#define CDB_FN_MAT_MUL 0x0511u
#define CDB_FN_MAT_TRANSPOSE 0x0512u
#define CDB_FN_MAT_DET 0x0513u
#define CDB_FN_MAT_TRACE 0x0514u

/* Spectral & SDR / DSP */
#define CDB_FN_FFT_MAGNITUDE 0x0600u
#define CDB_FN_FFT_POWER 0x0601u
#define CDB_FN_RFFT 0x0602u
#define CDB_FN_IFFT 0x0603u
#define CDB_FN_WINDOW_HANN 0x0604u
#define CDB_FN_WINDOW_HAMMING 0x0605u
#define CDB_FN_WINDOW_BLACKMAN 0x0606u
#define CDB_FN_IQ_MAGNITUDE 0x0607u
#define CDB_FN_IQ_PHASE 0x0608u
#define CDB_FN_IQ_DEMOD_FM 0x0609u
#define CDB_FN_DBFS 0x060Au
#define CDB_FN_FIR_FILTER 0x060Bu

/* Astronomy, Telescope Mount & Satellite Tracking (0x0700 - 0x071F) */
#define CDB_FN_ASTRO_ALT 0x0700u
#define CDB_FN_ASTRO_AZ 0x0701u
#define CDB_FN_ASTRO_RANGE 0x0702u
#define CDB_FN_ASTRO_ALTAZ_TO_RA 0x0703u
#define CDB_FN_ASTRO_ALTAZ_TO_DEC 0x0704u
#define CDB_FN_ASTRO_RADEC_TO_ALT 0x0705u
#define CDB_FN_ASTRO_RADEC_TO_AZ 0x0706u
#define CDB_FN_ASTRO_REFRACTION 0x0707u
#define CDB_FN_ASTRO_ANGULAR_SEP 0x0708u

/* Electronics & Circuit Engineering (0x0800 - 0x081F) */
#define CDB_FN_ELEC_THERMISTOR_NTC 0x0800u
#define CDB_FN_ELEC_STEINHART_HART 0x0801u
#define CDB_FN_ELEC_ADC_VOLTS 0x0802u
#define CDB_FN_ELEC_VOLTAGE_DIVIDER 0x0803u
#define CDB_FN_ELEC_PARALLEL_R 0x0804u
#define CDB_FN_ELEC_DBM_TO_WATTS 0x0805u
#define CDB_FN_ELEC_WATTS_TO_DBM 0x0806u
#define CDB_FN_ELEC_RC_CUTOFF 0x0807u
#define CDB_FN_ELEC_LC_RESONANCE 0x0808u
#define CDB_FN_ELEC_REACTANCE_C 0x0809u
#define CDB_FN_ELEC_REACTANCE_L 0x080Au

/* Atmospheric Science & Gas Body Modeling (0x0900 - 0x091F) */
#define CDB_FN_ATMO_PRESSURE_TO_ALT 0x0900u
#define CDB_FN_ATMO_ALT_TO_SEALEVEL_P 0x0901u
#define CDB_FN_ATMO_DEW_POINT 0x0902u
#define CDB_FN_ATMO_VAPOR_PRESSURE 0x0903u
#define CDB_FN_ATMO_HEAT_INDEX 0x0904u
#define CDB_FN_ATMO_WIND_CHILL 0x0905u
#define CDB_FN_ATMO_AIR_DENSITY 0x0906u
#define CDB_FN_GAS_IDEAL_PRESSURE 0x0907u

/* Fluid Dynamics & Thermal Flow (0x0A00 - 0x0A1F) */
#define CDB_FN_FLUID_DYNAMIC_PRESSURE 0x0A00u
#define CDB_FN_FLUID_PITOT_AIRSPEED 0x0A01u
#define CDB_FN_FLUID_REYNOLDS 0x0A02u
#define CDB_FN_FLUID_HYDROSTATIC_PRESSURE 0x0A03u
#define CDB_FN_FLUID_VENTURI_FLOW 0x0A04u
#define CDB_FN_THERMAL_JUNCTION_TEMP 0x0A05u
#define CDB_FN_THERMAL_RADIATION_POWER 0x0A06u

/* In-memory zero-copy view of an expression. */
typedef struct cdb_query_expr {
    uint8_t tag; /* CDB_QUERY_EXPR_* */
    union {
        struct {
            const uint8_t *data;
            size_t len;
        } literal;
        struct {
            const char *path;
            size_t len;
        } field;
        struct {
            uint8_t kind; /* CDB_PARAM_INDEX or CDB_PARAM_NAMED */
            uint32_t index;
            const char *name;
            size_t name_len;
        } param;
        struct {
            uint8_t op; /* CDB_UNARY_* */
            const uint8_t *operand_wire;
        } unary;
        struct {
            uint8_t op; /* CDB_BIN_* */
            const uint8_t *left_wire;
            const uint8_t *right_wire;
        } binary;
        struct {
            uint16_t fn_id;
            uint16_t n_args;
            const uint8_t *args_wire;
        } call;
    } as;
} cdb_query_expr;

/* In-memory zero-copy view of an operator node. */
typedef struct cdb_query_node {
    uint8_t tag; /* CDB_QUERY_NODE_* */
    union {
        struct {
            const char *name;
            size_t name_len;
        } scan;
        struct {
            const uint8_t *input_wire;
            const uint8_t *pred_wire;
        } filter;
        struct {
            const uint8_t *input_wire;
            int passthrough;
            size_t count;
            const uint8_t *outputs_wire;
        } project;
        struct {
            uint8_t kind; /* CDB_JOIN_* */
            const uint8_t *left_wire;
            const uint8_t *right_wire;
            const uint8_t *cond_wire;
        } join;
        struct {
            const uint8_t *input_wire;
            size_t count;
            const uint8_t *keys_wire;
        } sort;
        struct {
            const uint8_t *input_wire;
            uint64_t count;
            uint64_t offset;
        } limit;
        struct {
            size_t count;
            const uint8_t *branches_wire;
        } union_node;
        struct {
            const uint8_t *input_wire;
            size_t n_groups;
            const uint8_t *groups_wire;
            size_t n_aggs;
            const uint8_t *aggs_wire;
        } aggregate;
        struct {
            const uint8_t *input_wire;
            const char *link_type;
            size_t link_type_len;
            uint8_t dir;
            uint64_t min_hops;
            uint64_t max_hops;
        } traverse;
        struct {
            const uint8_t *input_wire;
            uint8_t mode;
            uint64_t limit;
            const char *query;
            size_t query_len;
            size_t n_fields;
            const uint8_t *fields_wire;
        } text_search;
        struct {
            const uint8_t *input_wire;
            const char *field;
            size_t field_len;
            const uint8_t *near_wire;
            uint64_t k;
            uint8_t metric;
        } vector_search;
        struct {
            uint8_t fuse;
            size_t count;
            const uint8_t *branches_wire;
        } hybrid_rank;
        struct {
            const uint8_t *input_wire;
            uint8_t type;
            const char *time_field;
            size_t time_field_len;
            uint64_t size_ns;
            uint64_t step_ns;
        } window;
        struct {
            uint8_t kind;
            const uint8_t *left_wire;
            const uint8_t *right_wire;
            const uint8_t *left_time_wire;
            const uint8_t *right_time_wire;
            const uint8_t *match_cond_wire;
            uint64_t tolerance_ns;
        } asof_join;
        struct {
            const uint8_t *input_wire;
            const uint8_t *array_expr_wire;
            const char *item_alias;
            size_t item_alias_len;
            const char *index_alias;
            size_t index_alias_len;
        } unnest;
    } as;
} cdb_query_node;

/* Streaming binary wire writer for CMBQL queries. */
typedef struct cdb_query_writer {
    uint8_t *buf;
    size_t cap;
    size_t len;
    cdb_status error;
} cdb_query_writer;

void cdb_query_writer_init(cdb_query_writer *w, uint8_t *buf, size_t cap);
cdb_status cdb_query_writer_begin(cdb_query_writer *w);
size_t cdb_query_writer_len(const cdb_query_writer *w);

/* Writer methods for plan nodes. */
cdb_status cdb_query_write_scan(cdb_query_writer *w, const char *store_name, size_t len);
cdb_status cdb_query_write_filter(cdb_query_writer *w);
cdb_status cdb_query_write_project(cdb_query_writer *w, int passthrough, size_t n_outputs);
cdb_status cdb_query_write_project_output(cdb_query_writer *w, const char *alias, size_t alias_len);
cdb_status cdb_query_write_join(cdb_query_writer *w, uint8_t kind);
cdb_status cdb_query_write_sort(cdb_query_writer *w, size_t n_keys);
cdb_status cdb_query_write_sort_key_suffix(cdb_query_writer *w, uint8_t dir, uint8_t nulls);
cdb_status cdb_query_write_limit(cdb_query_writer *w, uint64_t count, uint64_t offset);
cdb_status cdb_query_write_union(cdb_query_writer *w, size_t n_branches);
cdb_status cdb_query_write_aggregate(cdb_query_writer *w, size_t n_groups, size_t n_aggs);
cdb_status cdb_query_write_aggregate_output(cdb_query_writer *w, const char *alias, size_t alias_len, uint8_t fn);
cdb_status cdb_query_write_traverse(cdb_query_writer *w, const char *link_type, size_t len, uint8_t dir,
                                    uint64_t min_hops, uint64_t max_hops);
cdb_status cdb_query_write_text_search(cdb_query_writer *w, uint8_t mode, uint64_t limit,
                                      const char *query, size_t query_len, size_t n_fields);
cdb_status cdb_query_write_field_name(cdb_query_writer *w, const char *field, size_t field_len);
cdb_status cdb_query_write_vector_search(cdb_query_writer *w, const char *field, size_t field_len,
                                        uint64_t k, uint8_t metric);
cdb_status cdb_query_write_hybrid_rank(cdb_query_writer *w, uint8_t fuse, size_t n_branches);
cdb_status cdb_query_write_branch_weight(cdb_query_writer *w, uint32_t weight);
cdb_status cdb_query_write_window(cdb_query_writer *w, uint8_t type, const char *time_field, size_t len,
                                  uint64_t size_ns, uint64_t step_ns);
cdb_status cdb_query_write_asof_join(cdb_query_writer *w, uint8_t kind, uint64_t tolerance_ns);
cdb_status cdb_query_write_unnest(cdb_query_writer *w, const char *item_alias, size_t item_len,
                                  const char *index_alias, size_t idx_len);

/* Writer methods for expressions. */
cdb_status cdb_query_write_expr_literal(cdb_query_writer *w, const uint8_t *val_bytes, size_t val_len);
cdb_status cdb_query_write_expr_field(cdb_query_writer *w, const char *path, size_t path_len);
cdb_status cdb_query_write_expr_param_index(cdb_query_writer *w, uint32_t index);
cdb_status cdb_query_write_expr_param_named(cdb_query_writer *w, const char *name, size_t name_len);
cdb_status cdb_query_write_expr_unary(cdb_query_writer *w, uint8_t op);
cdb_status cdb_query_write_expr_binary(cdb_query_writer *w, uint8_t op);
cdb_status cdb_query_write_expr_call(cdb_query_writer *w, uint16_t fn_id, uint16_t n_args);

/* Validates wire bytes and verifies structural integrity.
 * CDB_E_INVALID_ARG for malformed wire, bad magic, or invalid bounds.
 * *root_offset reports where the root plan node begins. */
cdb_status cdb_query_validate(const uint8_t *wire, size_t len, size_t *root_offset);

/* Reads the operator node at *p and advances *p past the node header fields. */
cdb_status cdb_query_read_node(const uint8_t **p, const uint8_t *end, cdb_query_node *node);

/* Reads the expression at *p and advances *p past the expression header fields. */
cdb_status cdb_query_read_expr(const uint8_t **p, const uint8_t *end, cdb_query_expr *expr);

/* Skips past a complete operator node (and all its sub-nodes/expressions). */
cdb_status cdb_query_skip_node(const uint8_t **p, const uint8_t *end);

/* Skips past a complete expression (and all its sub-expressions). */
cdb_status cdb_query_skip_expr(const uint8_t **p, const uint8_t *end);

#endif
