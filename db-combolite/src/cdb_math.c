/* Zero-allocation mathematics, data science, DSP, and linear algebra engine (spec 11.10). */
#include "cdb_math.h"

#include <math.h>
#include <string.h>

#ifndef CDB_PI
#define CDB_PI 3.14159265358979323846
#endif

#define CDB_WGS84_RADIUS_METERS 6371008.8

/* ---------------------------------------------------------------- Trigonometry & Angles */

double cdb_math_sin(double x)
{
    return sin(x);
}

double cdb_math_cos(double x)
{
    return cos(x);
}

double cdb_math_tan(double x)
{
    return tan(x);
}

double cdb_math_asin(double x)
{
    if (x < -1.0 || x > 1.0)
        return 0.0;
    return asin(x);
}

double cdb_math_acos(double x)
{
    if (x < -1.0 || x > 1.0)
        return 0.0;
    return acos(x);
}

double cdb_math_atan(double x)
{
    return atan(x);
}

double cdb_math_atan2(double y, double x)
{
    return atan2(y, x);
}

double cdb_math_sinh(double x)
{
    return sinh(x);
}

double cdb_math_cosh(double x)
{
    return cosh(x);
}

double cdb_math_tanh(double x)
{
    return tanh(x);
}

double cdb_math_degrees(double radians)
{
    return radians * (180.0 / CDB_PI);
}

double cdb_math_radians(double degrees)
{
    return degrees * (CDB_PI / 180.0);
}

/* ---------------------------------------------------------------- Geometry & Geodesic */

double cdb_math_hypot(double x, double y)
{
    return hypot(x, y);
}

double cdb_math_hypot3(double x, double y, double z)
{
    return sqrt(x * x + y * y + z * z);
}

double cdb_math_haversine(double lat1, double lon1, double lat2, double lon2)
{
    double rlat1 = cdb_math_radians(lat1);
    double rlat2 = cdb_math_radians(lat2);
    double dlat = cdb_math_radians(lat2 - lat1);
    double dlon = cdb_math_radians(lon2 - lon1);

    double a = sin(dlat * 0.5) * sin(dlat * 0.5)
             + cos(rlat1) * cos(rlat2) * sin(dlon * 0.5) * sin(dlon * 0.5);
    if (a < 0.0)
        a = 0.0;
    if (a > 1.0)
        a = 1.0;
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return CDB_WGS84_RADIUS_METERS * c;
}

int cdb_math_bbox_contains(double min_x, double min_y, double max_x, double max_y, double x, double y)
{
    return (x >= min_x && x <= max_x && y >= min_y && y <= max_y);
}

/* ---------------------------------------------------------------- Navigation & GIS Projections */

double cdb_math_bearing(double lat1, double lon1, double lat2, double lon2)
{
    double phi1 = cdb_math_radians(lat1);
    double phi2 = cdb_math_radians(lat2);
    double dlon = cdb_math_radians(lon2 - lon1);

    double y = sin(dlon) * cos(phi2);
    double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlon);
    double theta = atan2(y, x);
    double deg = cdb_math_degrees(theta);
    return fmod(deg + 360.0, 360.0);
}

void cdb_math_destination(double lat1, double lon1, double bearing_deg, double dist_meters,
                          double *out_lat, double *out_lon)
{
    double delta = dist_meters / CDB_WGS84_RADIUS_METERS;
    double theta = cdb_math_radians(bearing_deg);
    double phi1 = cdb_math_radians(lat1);
    double lambda1 = cdb_math_radians(lon1);

    double sin_phi2 = sin(phi1) * cos(delta) + cos(phi1) * sin(delta) * cos(theta);
    double phi2 = asin(cdb_math_clamp(sin_phi2, -1.0, 1.0));
    double y = sin(theta) * sin(delta) * cos(phi1);
    double x = cos(delta) - sin(phi1) * sin(phi2);
    double lambda2 = lambda1 + atan2(y, x);

    if (out_lat != NULL)
        *out_lat = cdb_math_degrees(phi2);
    if (out_lon != NULL) {
        double deg_lon = cdb_math_degrees(lambda2);
        deg_lon = fmod(deg_lon + 540.0, 360.0) - 180.0;
        *out_lon = deg_lon;
    }
}

double cdb_math_cross_track_distance(double lat_start, double lon_start,
                                    double lat_end, double lon_end,
                                    double lat_p, double lon_p)
{
    double d13 = cdb_math_haversine(lat_start, lon_start, lat_p, lon_p) / CDB_WGS84_RADIUS_METERS;
    double theta13 = cdb_math_radians(cdb_math_bearing(lat_start, lon_start, lat_p, lon_p));
    double theta12 = cdb_math_radians(cdb_math_bearing(lat_start, lon_start, lat_end, lon_end));

    double xtd = asin(cdb_math_clamp(sin(d13) * sin(theta13 - theta12), -1.0, 1.0));
    return xtd * CDB_WGS84_RADIUS_METERS;
}

double cdb_math_along_track_distance(double lat_start, double lon_start,
                                    double lat_end, double lon_end,
                                    double lat_p, double lon_p)
{
    double d13 = cdb_math_haversine(lat_start, lon_start, lat_p, lon_p) / CDB_WGS84_RADIUS_METERS;
    double xtd = cdb_math_cross_track_distance(lat_start, lon_start, lat_end, lon_end, lat_p, lon_p) / CDB_WGS84_RADIUS_METERS;

    double cos_xtd = cos(xtd);
    double atd;
    if (fabs(cos_xtd) < 1e-12)
        return 0.0;
    atd = acos(cdb_math_clamp(cos(d13) / cos_xtd, -1.0, 1.0));
    return atd * CDB_WGS84_RADIUS_METERS;
}

int cdb_math_point_in_circle(double lat, double lon, double center_lat, double center_lon, double radius_meters)
{
    return cdb_math_haversine(lat, lon, center_lat, center_lon) <= radius_meters;
}

int cdb_math_point_in_polygon(double lat, double lon, const double *polygon_lats, const double *polygon_lons, size_t n_vertices)
{
    int inside = 0;
    size_t i, j;
    if (polygon_lats == NULL || polygon_lons == NULL || n_vertices < 3)
        return 0;

    for (i = 0, j = n_vertices - 1; i < n_vertices; j = i++) {
        double lat_i = polygon_lats[i];
        double lon_i = polygon_lons[i];
        double lat_j = polygon_lats[j];
        double lon_j = polygon_lons[j];

        if (((lat_i > lat) != (lat_j > lat))
            && (lon < (lon_j - lon_i) * (lat - lat_i) / (lat_j - lat_i) + lon_i)) {
            inside = !inside;
        }
    }
    return inside;
}

double cdb_math_wgs84_to_mercator_x(double lon)
{
    return CDB_WGS84_RADIUS_METERS * cdb_math_radians(lon);
}

double cdb_math_wgs84_to_mercator_y(double lat)
{
    double phi;
    lat = cdb_math_clamp(lat, -85.05112878, 85.05112878);
    phi = cdb_math_radians(lat);
    return CDB_WGS84_RADIUS_METERS * log(tan(CDB_PI * 0.25 + phi * 0.5));
}

double cdb_math_mercator_to_wgs84_lon(double x)
{
    return cdb_math_degrees(x / CDB_WGS84_RADIUS_METERS);
}

double cdb_math_mercator_to_wgs84_lat(double y)
{
    double phi = 2.0 * atan(exp(y / CDB_WGS84_RADIUS_METERS)) - CDB_PI * 0.5;
    return cdb_math_degrees(phi);
}

int cdb_math_lon_to_tile_x(double lon, int zoom)
{
    double n = pow(2.0, (double)zoom);
    int x = (int)floor((lon + 180.0) / 360.0 * n);
    return x < 0 ? 0 : x;
}

int cdb_math_lat_to_tile_y(double lat, int zoom)
{
    double phi = cdb_math_radians(lat);
    double n = pow(2.0, (double)zoom);
    double val = (1.0 - log(tan(phi) + 1.0 / cos(phi)) / CDB_PI) * 0.5 * n;
    int y = (int)floor(val);
    return y < 0 ? 0 : y;
}

double cdb_math_tile_x_to_lon(int tile_x, int zoom)
{
    double n = pow(2.0, (double)zoom);
    return (double)tile_x / n * 360.0 - 180.0;
}

double cdb_math_tile_y_to_lat(int tile_y, int zoom)
{
    double n = pow(2.0, (double)zoom);
    double val = CDB_PI * (1.0 - 2.0 * (double)tile_y / n);
    return cdb_math_degrees(atan(sinh(val)));
}

static uint32_t spread_bits_16(uint32_t x)
{
    x = (x | (x << 8)) & 0x00FF00FFu;
    x = (x | (x << 4)) & 0x0F0F0F0Fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

uint64_t cdb_math_morton2d(double lat, double lon)
{
    double nlat = cdb_math_clamp((lat + 90.0) / 180.0, 0.0, 1.0);
    double nlon = cdb_math_clamp((lon + 180.0) / 360.0, 0.0, 1.0);

    uint32_t y = (uint32_t)(nlat * 4294967295.0);
    uint32_t x = (uint32_t)(nlon * 4294967295.0);

    uint32_t x_lo = spread_bits_16(x & 0xFFFFu);
    uint32_t y_lo = spread_bits_16(y & 0xFFFFu);
    uint32_t x_hi = spread_bits_16((x >> 16) & 0xFFFFu);
    uint32_t y_hi = spread_bits_16((y >> 16) & 0xFFFFu);

    uint64_t lo = ((uint64_t)y_lo << 1) | (uint64_t)x_lo;
    uint64_t hi = ((uint64_t)y_hi << 1) | (uint64_t)x_hi;
    return (hi << 32) | lo;
}

static const char BASE32_CHARS[] = "0123456789bcdefghjkmnpqrstuvwxyz";

cdb_status cdb_math_geohash_encode(double lat, double lon, unsigned precision, char *out_buf, size_t cap)
{
    double min_lat = -90.0, max_lat = 90.0;
    double min_lon = -180.0, max_lon = 180.0;
    unsigned idx = 0;
    int is_even = 1;
    uint32_t ch = 0;
    unsigned bits = 0;

    if (out_buf == NULL || precision == 0 || precision > 12 || cap <= precision)
        return CDB_E_INVALID_ARG;

    while (idx < precision) {
        if (is_even) {
            double mid = (min_lon + max_lon) * 0.5;
            if (lon >= mid) {
                ch = (ch << 1) | 1u;
                min_lon = mid;
            } else {
                ch = (ch << 1);
                max_lon = mid;
            }
        } else {
            double mid = (min_lat + max_lat) * 0.5;
            if (lat >= mid) {
                ch = (ch << 1) | 1u;
                min_lat = mid;
            } else {
                ch = (ch << 1);
                max_lat = mid;
            }
        }
        is_even = !is_even;
        bits++;

        if (bits == 5) {
            out_buf[idx++] = BASE32_CHARS[ch];
            bits = 0;
            ch = 0;
        }
    }
    out_buf[idx] = '\0';
    return CDB_OK;
}

cdb_status cdb_math_geohash_decode(const char *geohash, size_t len, double *out_lat, double *out_lon)
{
    double min_lat = -90.0, max_lat = 90.0;
    double min_lon = -180.0, max_lon = 180.0;
    int is_even = 1;
    size_t i;
    int j;

    if (geohash == NULL || len == 0 || len > 12 || out_lat == NULL || out_lon == NULL)
        return CDB_E_INVALID_ARG;

    for (i = 0; i < len; i++) {
        char c = geohash[i];
        const char *p = strchr(BASE32_CHARS, c);
        int val;
        if (p == NULL)
            return CDB_E_INVALID_ARG;
        val = (int)(p - BASE32_CHARS);

        for (j = 4; j >= 0; j--) {
            int bit = (val >> j) & 1;
            if (is_even) {
                double mid = (min_lon + max_lon) * 0.5;
                if (bit)
                    min_lon = mid;
                else
                    max_lon = mid;
            } else {
                double mid = (min_lat + max_lat) * 0.5;
                if (bit)
                    min_lat = mid;
                else
                    max_lat = mid;
            }
            is_even = !is_even;
        }
    }

    *out_lat = (min_lat + max_lat) * 0.5;
    *out_lon = (min_lon + max_lon) * 0.5;
    return CDB_OK;
}

/* ---------------------------------------------------------------- Algebra & Activations */

double cdb_math_sqrt(double x)
{
    if (x < 0.0)
        return 0.0;
    return sqrt(x);
}

double cdb_math_cbrt(double x)
{
    return cbrt(x);
}

double cdb_math_pow(double x, double y)
{
    return pow(x, y);
}

double cdb_math_exp(double x)
{
    return exp(x);
}

double cdb_math_expm1(double x)
{
    return expm1(x);
}

double cdb_math_ln(double x)
{
    if (x <= 0.0)
        return 0.0;
    return log(x);
}

double cdb_math_log10(double x)
{
    if (x <= 0.0)
        return 0.0;
    return log10(x);
}

double cdb_math_log2(double x)
{
    if (x <= 0.0)
        return 0.0;
    return log2(x);
}

double cdb_math_log1p(double x)
{
    if (x <= -1.0)
        return 0.0;
    return log1p(x);
}

double cdb_math_abs(double x)
{
    return fabs(x);
}

double cdb_math_sign(double x)
{
    if (x > 0.0)
        return 1.0;
    if (x < 0.0)
        return -1.0;
    return 0.0;
}

double cdb_math_round(double x, int decimals)
{
    if (decimals <= 0)
        return round(x);
    {
        double factor = pow(10.0, (double)decimals);
        return round(x * factor) / factor;
    }
}

double cdb_math_floor(double x)
{
    return floor(x);
}

double cdb_math_ceil(double x)
{
    return ceil(x);
}

double cdb_math_clamp(double x, double min_val, double max_val)
{
    if (x < min_val)
        return min_val;
    if (x > max_val)
        return max_val;
    return x;
}

double cdb_math_lerp(double a, double b, double t)
{
    return a + t * (b - a);
}

double cdb_math_sigmoid(double x)
{
    if (x >= 45.0)
        return 1.0;
    if (x <= -45.0)
        return 0.0;
    return 1.0 / (1.0 + exp(-x));
}

double cdb_math_relu(double x)
{
    return x > 0.0 ? x : 0.0;
}

double cdb_math_gelu(double x)
{
    /* Gaussian Error Linear Unit (GELU) approximation */
    const double sqrt_2_over_pi = 0.7978845608028654;
    double inner = sqrt_2_over_pi * (x + 0.044715 * x * x * x);
    return 0.5 * x * (1.0 + tanh(inner));
}

double cdb_math_erf(double x)
{
    return erf(x);
}

/* ---------------------------------------------------------------- Calculus */

double cdb_math_diff(double curr, double prev)
{
    return curr - prev;
}

double cdb_math_derivative(double delta_y, double delta_t)
{
    if (delta_t == 0.0)
        return 0.0;
    return delta_y / delta_t;
}

double cdb_math_trapezoid(double y1, double y2, double delta_t)
{
    return (y1 + y2) * 0.5 * delta_t;
}

/* ---------------------------------------------------------------- SDR & Signal Processing */

double cdb_math_iq_magnitude(double i, double q)
{
    return hypot(i, q);
}

double cdb_math_iq_phase(double i, double q)
{
    return atan2(q, i);
}

double cdb_math_iq_demod_fm(double i_curr, double q_curr, double i_prev, double q_prev)
{
    /* Quadrature delay product: (I_k + j Q_k) * (I_{k-1} - j Q_{k-1})
     * real = I_k * I_{k-1} + Q_k * Q_{k-1}
     * imag = Q_k * I_{k-1} - I_k * Q_{k-1} */
    double re = i_curr * i_prev + q_curr * q_prev;
    double im = q_curr * i_prev - i_curr * q_prev;
    return atan2(im, re);
}

double cdb_math_dbfs(double power)
{
    if (power <= 1e-12)
        return -120.0;
    return 10.0 * log10(power);
}

void cdb_math_window_hann(float *out, size_t n)
{
    size_t i;
    if (out == NULL || n == 0)
        return;
    if (n == 1) {
        out[0] = 1.0f;
        return;
    }
    for (i = 0; i < n; i++) {
        double w = 0.5 * (1.0 - cos((2.0 * CDB_PI * (double)i) / (double)(n - 1)));
        out[i] = (float)w;
    }
}

void cdb_math_window_hamming(float *out, size_t n)
{
    size_t i;
    if (out == NULL || n == 0)
        return;
    if (n == 1) {
        out[0] = 1.0f;
        return;
    }
    for (i = 0; i < n; i++) {
        double w = 0.54 - 0.46 * cos((2.0 * CDB_PI * (double)i) / (double)(n - 1));
        out[i] = (float)w;
    }
}

void cdb_math_window_blackman(float *out, size_t n)
{
    size_t i;
    if (out == NULL || n == 0)
        return;
    if (n == 1) {
        out[0] = 1.0f;
        return;
    }
    for (i = 0; i < n; i++) {
        double a0 = 0.42;
        double a1 = 0.5;
        double a2 = 0.08;
        double f = (2.0 * CDB_PI * (double)i) / (double)(n - 1);
        double w = a0 - a1 * cos(f) + a2 * cos(2.0 * f);
        out[i] = (float)w;
    }
}

cdb_status cdb_math_fir_filter(const float *signal, size_t n, const float *taps, size_t n_taps, float *out)
{
    size_t i, j;
    if (signal == NULL || taps == NULL || out == NULL || n_taps == 0)
        return CDB_E_INVALID_ARG;

    for (i = 0; i < n; i++) {
        float acc = 0.0f;
        for (j = 0; j < n_taps; j++) {
            if (i >= j)
                acc += taps[j] * signal[i - j];
        }
        out[i] = acc;
    }
    return CDB_OK;
}

/* ---------------------------------------------------------------- Spectral & FFT */

static unsigned calc_log2(size_t n)
{
    unsigned bits = 0;
    while ((n >>= 1) > 0)
        bits++;
    return bits;
}

static size_t bit_reverse(size_t x, unsigned bits)
{
    size_t res = 0;
    unsigned i;
    for (i = 0; i < bits; i++) {
        res = (res << 1) | (x & 1u);
        x >>= 1;
    }
    return res;
}

cdb_status cdb_math_fft(float *real, float *imag, size_t n, int inverse)
{
    unsigned bits;
    size_t i, len;

    if (real == NULL || imag == NULL || n < 2 || (n & (n - 1)) != 0)
        return CDB_E_INVALID_ARG;

    bits = calc_log2(n);

    /* Bit-reversal permutation */
    for (i = 0; i < n; i++) {
        size_t rev = bit_reverse(i, bits);
        if (rev > i) {
            float tr = real[i];
            float ti = imag[i];
            real[i] = real[rev];
            imag[i] = imag[rev];
            real[rev] = tr;
            imag[rev] = ti;
        }
    }

    /* Cooley-Tukey butterfly stages */
    for (len = 2; len <= n; len <<= 1) {
        double theta = (inverse ? 2.0 : -2.0) * CDB_PI / (double)len;
        double wreal = cos(theta);
        double wimag = sin(theta);
        size_t half = len >> 1;

        for (i = 0; i < n; i += len) {
            double u_r = 1.0;
            double u_i = 0.0;
            size_t j;

            for (j = 0; j < half; j++) {
                size_t u_idx = i + j;
                size_t v_idx = i + j + half;

                double vr = (double)real[v_idx];
                double vi = (double)imag[v_idx];

                double tr = vr * u_r - vi * u_i;
                double ti = vr * u_i + vi * u_r;

                double ur = (double)real[u_idx];
                double ui = (double)imag[u_idx];

                real[u_idx] = (float)(ur + tr);
                imag[u_idx] = (float)(ui + ti);
                real[v_idx] = (float)(ur - tr);
                imag[v_idx] = (float)(ui - ti);

                {
                    double next_ur = u_r * wreal - u_i * wimag;
                    u_i = u_r * wimag + u_i * wreal;
                    u_r = next_ur;
                }
            }
        }
    }

    /* Normalization for inverse FFT */
    if (inverse) {
        float inv_n = 1.0f / (float)n;
        for (i = 0; i < n; i++) {
            real[i] *= inv_n;
            imag[i] *= inv_n;
        }
    }

    return CDB_OK;
}

cdb_status cdb_math_fft_magnitude(const float *signal, size_t n, float *work_real, float *work_imag, float *out_mag)
{
    cdb_status s;
    size_t k, half;

    if (signal == NULL || work_real == NULL || work_imag == NULL || out_mag == NULL)
        return CDB_E_INVALID_ARG;

    memcpy(work_real, signal, n * sizeof(float));
    memset(work_imag, 0, n * sizeof(float));

    if ((s = cdb_math_fft(work_real, work_imag, n, 0)) != CDB_OK)
        return s;

    half = n / 2;
    for (k = 0; k <= half; k++) {
        double r = (double)work_real[k];
        double im = (double)work_imag[k];
        out_mag[k] = (float)hypot(r, im);
    }

    return CDB_OK;
}

cdb_status cdb_math_fft_power(const float *signal, size_t n, float *work_real, float *work_imag, float *out_power)
{
    cdb_status s;
    size_t k, half;
    float norm;

    if (signal == NULL || work_real == NULL || work_imag == NULL || out_power == NULL || n == 0)
        return CDB_E_INVALID_ARG;

    memcpy(work_real, signal, n * sizeof(float));
    memset(work_imag, 0, n * sizeof(float));

    if ((s = cdb_math_fft(work_real, work_imag, n, 0)) != CDB_OK)
        return s;

    half = n / 2;
    norm = 1.0f / (float)n;
    for (k = 0; k <= half; k++) {
        float r = work_real[k];
        float im = work_imag[k];
        out_power[k] = (r * r + im * im) * norm;
    }

    return CDB_OK;
}

/* ---------------------------------------------------------------- Linear Algebra */

cdb_status cdb_math_vec_dot(const float *u, const float *v, size_t n, float *out)
{
    size_t i;
    double acc = 0.0;
    if (u == NULL || v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    for (i = 0; i < n; i++)
        acc += (double)u[i] * (double)v[i];

    *out = (float)acc;
    return CDB_OK;
}

cdb_status cdb_math_vec_norm(const float *v, size_t n, float *out)
{
    float dot = 0.0f;
    cdb_status s;
    if ((s = cdb_math_vec_dot(v, v, n, &dot)) != CDB_OK)
        return s;
    *out = sqrtf(dot);
    return CDB_OK;
}

cdb_status cdb_math_vec_cosine(const float *u, const float *v, size_t n, float *out)
{
    float dot = 0.0f, nu = 0.0f, nv = 0.0f;
    cdb_status s;
    if ((s = cdb_math_vec_dot(u, v, n, &dot)) != CDB_OK
        || (s = cdb_math_vec_norm(u, n, &nu)) != CDB_OK
        || (s = cdb_math_vec_norm(v, n, &nv)) != CDB_OK)
        return s;

    if (nu == 0.0f || nv == 0.0f) {
        *out = 0.0f;
        return CDB_OK;
    }

    *out = dot / (nu * nv);
    return CDB_OK;
}

cdb_status cdb_math_vec_distance(const float *u, const float *v, size_t n, float *out)
{
    size_t i;
    double acc = 0.0;
    if (u == NULL || v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    for (i = 0; i < n; i++) {
        double diff = (double)u[i] - (double)v[i];
        acc += diff * diff;
    }

    *out = (float)sqrt(acc);
    return CDB_OK;
}

cdb_status cdb_math_vec_scale(const float *v, float s, size_t n, float *out)
{
    size_t i;
    if (v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < n; i++)
        out[i] = v[i] * s;
    return CDB_OK;
}

cdb_status cdb_math_vec_add(const float *u, const float *v, size_t n, float *out)
{
    size_t i;
    if (u == NULL || v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < n; i++)
        out[i] = u[i] + v[i];
    return CDB_OK;
}

cdb_status cdb_math_vec_sub(const float *u, const float *v, size_t n, float *out)
{
    size_t i;
    if (u == NULL || v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < n; i++)
        out[i] = u[i] - v[i];
    return CDB_OK;
}

cdb_status cdb_math_mat_vec_mul(const float *m, size_t rows, size_t cols, const float *v, float *out)
{
    size_t r, c;
    if (m == NULL || v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    for (r = 0; r < rows; r++) {
        double acc = 0.0;
        for (c = 0; c < cols; c++)
            acc += (double)m[r * cols + c] * (double)v[c];
        out[r] = (float)acc;
    }
    return CDB_OK;
}

cdb_status cdb_math_mat_mul(const float *a, size_t a_rows, size_t a_cols, const float *b, size_t b_cols, float *out)
{
    size_t r, c, k;
    if (a == NULL || b == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    for (r = 0; r < a_rows; r++) {
        for (c = 0; c < b_cols; c++) {
            double acc = 0.0;
            for (k = 0; k < a_cols; k++)
                acc += (double)a[r * a_cols + k] * (double)b[k * b_cols + c];
            out[r * b_cols + c] = (float)acc;
        }
    }
    return CDB_OK;
}

cdb_status cdb_math_mat_transpose(const float *m, size_t rows, size_t cols, float *out)
{
    size_t r, c;
    if (m == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++)
            out[c * rows + r] = m[r * cols + c];
    }
    return CDB_OK;
}

cdb_status cdb_math_mat_det(const float *m, size_t dim, float *out)
{
    if (m == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    if (dim == 1) {
        *out = m[0];
        return CDB_OK;
    }
    if (dim == 2) {
        /* a*d - b*c */
        double a = (double)m[0];
        double b = (double)m[1];
        double c = (double)m[2];
        double d = (double)m[3];
        *out = (float)(a * d - b * c);
        return CDB_OK;
    }
    if (dim == 3) {
        /* Rule of Sarrus */
        double a = (double)m[0], b = (double)m[1], c = (double)m[2];
        double d = (double)m[3], e = (double)m[4], f = (double)m[5];
        double g = (double)m[6], h = (double)m[7], i = (double)m[8];
        double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        *out = (float)det;
        return CDB_OK;
    }

    return CDB_E_UNSUPPORTED;
}

cdb_status cdb_math_mat_trace(const float *m, size_t dim, float *out)
{
    size_t i;
    double acc = 0.0;
    if (m == NULL || out == NULL)
        return CDB_E_INVALID_ARG;

    for (i = 0; i < dim; i++)
        acc += (double)m[i * dim + i];

    *out = (float)acc;
    return CDB_OK;
}

/* ---------------------------------------------------------------- Astronomy & Telescope Mount */

static void geodetic_to_ecef(double lat_deg, double lon_deg, double alt_m,
                             double *x, double *y, double *z)
{
    const double a = 6378137.0; /* WGS84 semi-major axis */
    const double e2 = 0.00669437999014; /* eccentricity squared */

    double phi = cdb_math_radians(lat_deg);
    double lambda = cdb_math_radians(lon_deg);
    double sin_phi = sin(phi);
    double cos_phi = cos(phi);
    double n = a / sqrt(1.0 - e2 * sin_phi * sin_phi);

    *x = (n + alt_m) * cos_phi * cos(lambda);
    *y = (n + alt_m) * cos_phi * sin(lambda);
    *z = (n * (1.0 - e2) + alt_m) * sin_phi;
}

void cdb_math_astro_topocentric(double obs_lat, double obs_lon, double obs_alt,
                                double tgt_lat, double tgt_lon, double tgt_alt,
                                double *out_az, double *out_alt, double *out_range)
{
    double ox, oy, oz;
    double tx, ty, tz;
    double dx, dy, dz;
    double phi_o, lam_o;
    double sin_phi, cos_phi, sin_lam, cos_lam;
    double e, n, u;
    double range;

    geodetic_to_ecef(obs_lat, obs_lon, obs_alt, &ox, &oy, &oz);
    geodetic_to_ecef(tgt_lat, tgt_lon, tgt_alt, &tx, &ty, &tz);

    dx = tx - ox;
    dy = ty - oy;
    dz = tz - oz;

    phi_o = cdb_math_radians(obs_lat);
    lam_o = cdb_math_radians(obs_lon);
    sin_phi = sin(phi_o);
    cos_phi = cos(phi_o);
    sin_lam = sin(lam_o);
    cos_lam = cos(lam_o);

    /* Transform ECEF vector to local ENU */
    e = -sin_lam * dx + cos_lam * dy;
    n = -sin_phi * cos_lam * dx - sin_phi * sin_lam * dy + cos_phi * dz;
    u = cos_phi * cos_lam * dx + cos_phi * sin_lam * dy + sin_phi * dz;

    range = sqrt(e * e + n * n + u * u);
    if (out_range != NULL)
        *out_range = range;

    if (out_az != NULL) {
        double az = cdb_math_degrees(atan2(e, n));
        *out_az = fmod(az + 360.0, 360.0);
    }

    if (out_alt != NULL) {
        double horiz = sqrt(e * e + n * n);
        *out_alt = cdb_math_degrees(atan2(u, horiz));
    }
}

static double calc_lst_deg(double obs_lon, int64_t timestamp_ns)
{
    double jd = 2440587.5 + ((double)timestamp_ns / 1e9) / 86400.0;
    double d = jd - 2451545.0;
    double gmst = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    if (gmst < 0.0)
        gmst += 360.0;
    return fmod(gmst + obs_lon + 360.0, 360.0);
}

void cdb_math_astro_altaz_to_radec(double alt_deg, double az_deg, double obs_lat, double obs_lon,
                                   int64_t timestamp_ns, double *out_ra, double *out_dec)
{
    double phi = cdb_math_radians(obs_lat);
    double alt = cdb_math_radians(alt_deg);
    double az = cdb_math_radians(az_deg);

    double sin_dec = sin(phi) * sin(alt) + cos(phi) * cos(alt) * cos(az);
    double dec = asin(cdb_math_clamp(sin_dec, -1.0, 1.0));
    double cos_dec = cos(dec);

    double cos_h = (sin(alt) - sin(phi) * sin(dec)) / (cos(phi) * cos_dec + 1e-15);
    double sin_h = -(sin(az) * cos(alt)) / (cos_dec + 1e-15);
    double h = cdb_math_degrees(atan2(sin_h, cos_h));
    double lst = calc_lst_deg(obs_lon, timestamp_ns);

    if (out_dec != NULL)
        *out_dec = cdb_math_degrees(dec);
    if (out_ra != NULL)
        *out_ra = fmod(lst - h + 720.0, 360.0);
}

void cdb_math_astro_radec_to_altaz(double ra_deg, double dec_deg, double obs_lat, double obs_lon,
                                   int64_t timestamp_ns, double *out_alt, double *out_az)
{
    double lst = calc_lst_deg(obs_lon, timestamp_ns);
    double h = cdb_math_radians(fmod(lst - ra_deg + 720.0, 360.0));
    double phi = cdb_math_radians(obs_lat);
    double dec = cdb_math_radians(dec_deg);

    double sin_alt = sin(phi) * sin(dec) + cos(phi) * cos(dec) * cos(h);
    double alt = asin(cdb_math_clamp(sin_alt, -1.0, 1.0));

    double y = -sin(h) * cos(dec);
    double x = sin(dec) * cos(phi) - cos(dec) * sin(phi) * cos(h);
    double az = cdb_math_degrees(atan2(y, x));

    if (out_alt != NULL)
        *out_alt = cdb_math_degrees(alt);
    if (out_az != NULL)
        *out_az = fmod(az + 360.0, 360.0);
}

double cdb_math_astro_refraction(double alt_deg)
{
    double r;
    if (alt_deg < -5.0)
        return 0.0;
    r = 1.02 / tan(cdb_math_radians(alt_deg + 10.3 / (alt_deg + 5.11)));
    return (r / 60.0);
}

double cdb_math_astro_angular_sep(double ra1, double dec1, double ra2, double dec2)
{
    double r1 = cdb_math_radians(ra1);
    double d1 = cdb_math_radians(dec1);
    double r2 = cdb_math_radians(ra2);
    double d2 = cdb_math_radians(dec2);

    double cos_sep = sin(d1) * sin(d2) + cos(d1) * cos(d2) * cos(r1 - r2);
    return cdb_math_degrees(acos(cdb_math_clamp(cos_sep, -1.0, 1.0)));
}

/* ---------------------------------------------------------------- Electronics & Circuits */

double cdb_math_elec_thermistor_ntc(double r_measured, double r0, double beta, double t0_c)
{
    double t0_k = t0_c + 273.15;
    double inv_t;
    if (r_measured <= 0.0 || r0 <= 0.0 || beta <= 0.0)
        return 0.0;
    inv_t = (1.0 / t0_k) + (1.0 / beta) * log(r_measured / r0);
    return (1.0 / inv_t) - 273.15;
}

double cdb_math_elec_steinhart_hart(double r, double a, double b, double c)
{
    double ln_r;
    double inv_t;
    if (r <= 0.0)
        return 0.0;
    ln_r = log(r);
    inv_t = a + b * ln_r + c * ln_r * ln_r * ln_r;
    if (inv_t <= 0.0)
        return 0.0;
    return (1.0 / inv_t) - 273.15;
}

double cdb_math_elec_adc_volts(double raw_count, double max_count, double vref)
{
    if (max_count <= 0.0)
        return 0.0;
    return (raw_count / max_count) * vref;
}

double cdb_math_elec_voltage_divider(double vin, double r1, double r2)
{
    if (r1 + r2 <= 0.0)
        return 0.0;
    return vin * (r2 / (r1 + r2));
}

double cdb_math_elec_parallel_r(double r1, double r2)
{
    if (r1 + r2 == 0.0)
        return 0.0;
    return (r1 * r2) / (r1 + r2);
}

double cdb_math_elec_dbm_to_watts(double dbm)
{
    return 0.001 * pow(10.0, dbm * 0.1);
}

double cdb_math_elec_watts_to_dbm(double watts)
{
    if (watts <= 1e-15)
        return -120.0;
    return 10.0 * log10(watts / 0.001);
}

double cdb_math_elec_rc_cutoff(double r, double c)
{
    if (r <= 0.0 || c <= 0.0)
        return 0.0;
    return 1.0 / (2.0 * CDB_PI * r * c);
}

double cdb_math_elec_lc_resonance(double l, double c)
{
    if (l <= 0.0 || c <= 0.0)
        return 0.0;
    return 1.0 / (2.0 * CDB_PI * sqrt(l * c));
}

double cdb_math_elec_reactance_c(double f, double c)
{
    if (f <= 0.0 || c <= 0.0)
        return 0.0;
    return 1.0 / (2.0 * CDB_PI * f * c);
}

double cdb_math_elec_reactance_l(double f, double l)
{
    return 2.0 * CDB_PI * f * l;
}

/* ---------------------------------------------------------------- Atmospheric & Gas */

double cdb_math_atmo_pressure_to_alt(double p_hpa, double p0_hpa)
{
    if (p_hpa <= 0.0 || p0_hpa <= 0.0)
        return 0.0;
    return 44330.77 * (1.0 - pow(p_hpa / p0_hpa, 0.190263));
}

double cdb_math_atmo_alt_to_sealevel_p(double p_hpa, double alt_m, double temp_c)
{
    double t_k = temp_c + 273.15;
    double factor;
    if (p_hpa <= 0.0 || t_k <= 0.0)
        return 0.0;
    factor = 1.0 - (0.0065 * alt_m) / (t_k + 0.0065 * alt_m);
    if (factor <= 0.0)
        return 0.0;
    return p_hpa * pow(factor, -5.25588);
}

double cdb_math_atmo_dew_point(double temp_c, double rh_percent)
{
    const double a = 17.27;
    const double b = 237.7;
    double gamma;
    if (rh_percent <= 0.0)
        rh_percent = 0.001;
    if (rh_percent > 100.0)
        rh_percent = 100.0;
    gamma = (a * temp_c) / (b + temp_c) + log(rh_percent / 100.0);
    return (b * gamma) / (a - gamma);
}

double cdb_math_atmo_vapor_pressure(double temp_c, double rh_percent)
{
    double es = 6.112 * exp((17.67 * temp_c) / (temp_c + 243.5));
    return es * (rh_percent / 100.0);
}

double cdb_math_atmo_heat_index(double temp_c, double rh_percent)
{
    /* Rothfusz regression equation in Fahrenheit, converted to/from Celsius */
    double t = temp_c * 1.8 + 32.0;
    double r = rh_percent;
    double hi;

    if (t < 80.0)
        return 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + r * 0.094);

    hi = -42.379 + 2.04901523 * t + 10.14333127 * r
         - 0.22475541 * t * r - 0.00683783 * t * t
         - 0.05481717 * r * r + 0.00122874 * t * t * r
         + 0.00085282 * t * r * r - 0.00000199 * t * t * r * r;

    return (hi - 32.0) / 1.8;
}

double cdb_math_atmo_wind_chill(double temp_c, double wind_mps)
{
    double v_kmh = wind_mps * 3.6;
    if (temp_c > 10.0 || v_kmh < 4.8)
        return temp_c;
    return 13.12 + 0.6215 * temp_c - 11.37 * pow(v_kmh, 0.16) + 0.3965 * temp_c * pow(v_kmh, 0.16);
}

double cdb_math_atmo_air_density(double p_pa, double temp_c, double rh_percent)
{
    const double rd = 287.058; /* dry air gas constant J/(kg*K) */
    const double rv = 461.495; /* water vapor gas constant */
    double t_k = temp_c + 273.15;
    double pv_pa = cdb_math_atmo_vapor_pressure(temp_c, rh_percent) * 100.0;
    double pd_pa = p_pa - pv_pa;

    if (t_k <= 0.0)
        return 0.0;
    return (pd_pa / (rd * t_k)) + (pv_pa / (rv * t_k));
}

double cdb_math_gas_ideal_pressure(double n_moles, double temp_k, double volume_m3)
{
    const double r = 8.314462618;
    if (volume_m3 <= 0.0)
        return 0.0;
    return (n_moles * r * temp_k) / volume_m3;
}

/* ---------------------------------------------------------------- Fluid Dynamics & Thermal */

double cdb_math_fluid_dynamic_pressure(double density, double velocity)
{
    return 0.5 * density * velocity * velocity;
}

double cdb_math_fluid_pitot_airspeed(double total_p_pa, double static_p_pa, double density)
{
    double delta_p = total_p_pa - static_p_pa;
    if (delta_p <= 0.0 || density <= 0.0)
        return 0.0;
    return sqrt((2.0 * delta_p) / density);
}

double cdb_math_fluid_reynolds(double density, double velocity, double diameter, double viscosity)
{
    if (viscosity <= 0.0)
        return 0.0;
    return (density * velocity * diameter) / viscosity;
}

double cdb_math_fluid_hydrostatic_pressure(double density, double depth_m)
{
    const double g = 9.80665;
    return density * g * depth_m;
}

double cdb_math_fluid_venturi_flow(double delta_p_pa, double density, double cd, double area1, double area2)
{
    double beta;
    if (area1 <= 0.0 || area2 <= 0.0 || density <= 0.0 || delta_p_pa <= 0.0)
        return 0.0;
    beta = area2 / area1;
    return cd * area2 * sqrt((2.0 * delta_p_pa) / (density * (1.0 - beta * beta * beta * beta)));
}

double cdb_math_thermal_junction_temp(double ambient_c, double power_watts, double theta_ja)
{
    return ambient_c + power_watts * theta_ja;
}

double cdb_math_thermal_radiation_power(double emissivity, double area_m2, double temp_k)
{
    const double sigma = 5.670374419e-8;
    return emissivity * sigma * area_m2 * pow(temp_k, 4.0);
}
