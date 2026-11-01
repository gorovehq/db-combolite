/* Zero-allocation mathematics, data science, DSP, and linear algebra engine (spec 11.10).
 *
 * Implements:
 *   - Scalar mathematics: Trigonometry, Hyperbolics, Angles.
 *   - Geometry: Euclidean distances (2D/3D), Geodesic Haversine (WGS84).
 *   - Algebra, Roots, Powers, Logarithms & Neural Activations (Sigmoid, ReLU, GELU, Erf).
 *   - Calculus & Numerical Analysis: Differences, Derivatives, Trapezoidal Integration.
 *   - SDR / DSP & Signal Processing: I/Q Envelope, Instantaneous Phase, FM Demodulation,
 *     dBFS, FIR Filtering, and Windowing (Hann, Hamming, Blackman).
 *   - Spectral Analysis: Radix-2 Cooley-Tukey FFT, RFFT, IFFT, Magnitude & Power Spectrum.
 *   - Linear Algebra: Vector Dot, Norm, Cosine, Distance, Scale, Add/Sub,
 *     Matrix Multiplication, Transpose, Determinants, and Trace.
 *
 * All operations are zero-heap: caller provides memory or scratch buffers. */
#ifndef CDB_MATH_H
#define CDB_MATH_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

/* ---------------------------------------------------------------- Trigonometry & Angles */
double cdb_math_sin(double x);
double cdb_math_cos(double x);
double cdb_math_tan(double x);
double cdb_math_asin(double x);
double cdb_math_acos(double x);
double cdb_math_atan(double x);
double cdb_math_atan2(double y, double x);
double cdb_math_sinh(double x);
double cdb_math_cosh(double x);
double cdb_math_tanh(double x);
double cdb_math_degrees(double radians);
double cdb_math_radians(double degrees);

/* ---------------------------------------------------------------- Geometry & Geodesic */
double cdb_math_hypot(double x, double y);
double cdb_math_hypot3(double x, double y, double z);
/* Great-circle distance between two (lat, lon) pairs in degrees; returns meters on WGS84 sphere. */
double cdb_math_haversine(double lat1, double lon1, double lat2, double lon2);
int cdb_math_bbox_contains(double min_x, double min_y, double max_x, double max_y, double x, double y);

/* ---------------------------------------------------------------- Navigation & GIS Projections */
/* Initial bearing (forward azimuth) from (lat1, lon1) to (lat2, lon2) in degrees [0, 360). */
double cdb_math_bearing(double lat1, double lon1, double lat2, double lon2);

/* Destination point given start point, initial bearing (degrees), and distance (meters). */
void cdb_math_destination(double lat1, double lon1, double bearing_deg, double dist_meters,
                          double *out_lat, double *out_lon);

/* Cross-track distance (perpendicular deviation from great-circle path) in meters.
 * Path is defined from (lat_start, lon_start) to (lat_end, lon_end). Point is (lat_p, lon_p). */
double cdb_math_cross_track_distance(double lat_start, double lon_start,
                                    double lat_end, double lon_end,
                                    double lat_p, double lon_p);

/* Along-track distance (distance along path from start to closest point) in meters. */
double cdb_math_along_track_distance(double lat_start, double lon_start,
                                    double lat_end, double lon_end,
                                    double lat_p, double lon_p);

/* Geofencing: checks if (lat, lon) is inside a circle of radius meters. */
int cdb_math_point_in_circle(double lat, double lon, double center_lat, double center_lon, double radius_meters);

/* Geofencing: Ray-casting point-in-polygon test for arbitrary 2D polygon vertices.
 * polygon_lats and polygon_lons have n_vertices elements. Returns 1 if inside, 0 if outside. */
int cdb_math_point_in_polygon(double lat, double lon, const double *polygon_lats, const double *polygon_lons, size_t n_vertices);

/* Web Mercator (EPSG:3857) projection: WGS84 (lat, lon) in degrees to planar meters (x, y). */
double cdb_math_wgs84_to_mercator_x(double lon);
double cdb_math_wgs84_to_mercator_y(double lat);
double cdb_math_mercator_to_wgs84_lon(double x);
double cdb_math_mercator_to_wgs84_lat(double y);

/* Map slippy tile coordinates at zoom level z. */
int cdb_math_lon_to_tile_x(double lon, int zoom);
int cdb_math_lat_to_tile_y(double lat, int zoom);
double cdb_math_tile_x_to_lon(int tile_x, int zoom);
double cdb_math_tile_y_to_lat(int tile_y, int zoom);

/* 64-bit Morton code (Z-order bit interleaving) of normalized (lat, lon) for spatial B+tree indexing. */
uint64_t cdb_math_morton2d(double lat, double lon);

/* Geohash encoding (base32 string, up to 12 chars). out_buf must have cap >= precision + 1. */
cdb_status cdb_math_geohash_encode(double lat, double lon, unsigned precision, char *out_buf, size_t cap);
cdb_status cdb_math_geohash_decode(const char *geohash, size_t len, double *out_lat, double *out_lon);

/* ---------------------------------------------------------------- Algebra & Activations */
double cdb_math_sqrt(double x);
double cdb_math_cbrt(double x);
double cdb_math_pow(double x, double y);
double cdb_math_exp(double x);
double cdb_math_expm1(double x);
double cdb_math_ln(double x);
double cdb_math_log10(double x);
double cdb_math_log2(double x);
double cdb_math_log1p(double x);
double cdb_math_abs(double x);
double cdb_math_sign(double x);
double cdb_math_round(double x, int decimals);
double cdb_math_floor(double x);
double cdb_math_ceil(double x);
double cdb_math_clamp(double x, double min_val, double max_val);
double cdb_math_lerp(double a, double b, double t);
double cdb_math_sigmoid(double x);
double cdb_math_relu(double x);
double cdb_math_gelu(double x);
double cdb_math_erf(double x);

/* ---------------------------------------------------------------- Calculus */
double cdb_math_diff(double curr, double prev);
double cdb_math_derivative(double delta_y, double delta_t);
double cdb_math_trapezoid(double y1, double y2, double delta_t);

/* ---------------------------------------------------------------- SDR & Signal Processing */
double cdb_math_iq_magnitude(double i, double q);
double cdb_math_iq_phase(double i, double q);
/* Quadrature delay discriminator for direct FM demodulation. */
double cdb_math_iq_demod_fm(double i_curr, double q_curr, double i_prev, double q_prev);
/* Power to dBFS relative to 1.0 (with -120 dB noise floor clamp). */
double cdb_math_dbfs(double power);

/* Windowing functions for spectral analysis. */
void cdb_math_window_hann(float *out, size_t n);
void cdb_math_window_hamming(float *out, size_t n);
void cdb_math_window_blackman(float *out, size_t n);

/* Finite Impulse Response (FIR) filter: direct zero-allocation convolution. */
cdb_status cdb_math_fir_filter(const float *signal, size_t n, const float *taps, size_t n_taps, float *out);

/* ---------------------------------------------------------------- Spectral & FFT */
/* In-place radix-2 Cooley-Tukey complex FFT.
 * n must be a power of 2 (n >= 2). inverse = 0 for forward, 1 for inverse. */
cdb_status cdb_math_fft(float *real, float *imag, size_t n, int inverse);

/* Single-sided amplitude spectrum of a real-valued signal.
 * signal has length n (power of 2).
 * work_real and work_imag must each have length at least n.
 * out_mag must have length at least n/2 + 1. */
cdb_status cdb_math_fft_magnitude(const float *signal, size_t n, float *work_real, float *work_imag, float *out_mag);

/* Single-sided power spectral density (PSD) of a real-valued signal.
 * signal has length n (power of 2).
 * work_real and work_imag must each have length at least n.
 * out_power must have length at least n/2 + 1. */
cdb_status cdb_math_fft_power(const float *signal, size_t n, float *work_real, float *work_imag, float *out_power);

/* ---------------------------------------------------------------- Linear Algebra */
cdb_status cdb_math_vec_dot(const float *u, const float *v, size_t n, float *out);
cdb_status cdb_math_vec_norm(const float *v, size_t n, float *out);
cdb_status cdb_math_vec_cosine(const float *u, const float *v, size_t n, float *out);
cdb_status cdb_math_vec_distance(const float *u, const float *v, size_t n, float *out);
cdb_status cdb_math_vec_scale(const float *v, float s, size_t n, float *out);
cdb_status cdb_math_vec_add(const float *u, const float *v, size_t n, float *out);
cdb_status cdb_math_vec_sub(const float *u, const float *v, size_t n, float *out);

/* Matrix operations: row-major layout. */
cdb_status cdb_math_mat_vec_mul(const float *m, size_t rows, size_t cols, const float *v, float *out);
cdb_status cdb_math_mat_mul(const float *a, size_t a_rows, size_t a_cols, const float *b, size_t b_cols, float *out);
cdb_status cdb_math_mat_transpose(const float *m, size_t rows, size_t cols, float *out);
cdb_status cdb_math_mat_det(const float *m, size_t dim, float *out);
cdb_status cdb_math_mat_trace(const float *m, size_t dim, float *out);

/* ---------------------------------------------------------------- Astronomy & Telescope Mount */
void cdb_math_astro_topocentric(double obs_lat, double obs_lon, double obs_alt,
                                double tgt_lat, double tgt_lon, double tgt_alt,
                                double *out_az, double *out_alt, double *out_range);
void cdb_math_astro_altaz_to_radec(double alt_deg, double az_deg, double obs_lat, double obs_lon,
                                   int64_t timestamp_ns, double *out_ra, double *out_dec);
void cdb_math_astro_radec_to_altaz(double ra_deg, double dec_deg, double obs_lat, double obs_lon,
                                   int64_t timestamp_ns, double *out_alt, double *out_az);
double cdb_math_astro_refraction(double alt_deg);
double cdb_math_astro_angular_sep(double ra1, double dec1, double ra2, double dec2);

/* ---------------------------------------------------------------- Electronics & Circuits */
double cdb_math_elec_thermistor_ntc(double r_measured, double r0, double beta, double t0_c);
double cdb_math_elec_steinhart_hart(double r, double a, double b, double c);
double cdb_math_elec_adc_volts(double raw_count, double max_count, double vref);
double cdb_math_elec_voltage_divider(double vin, double r1, double r2);
double cdb_math_elec_parallel_r(double r1, double r2);
double cdb_math_elec_dbm_to_watts(double dbm);
double cdb_math_elec_watts_to_dbm(double watts);
double cdb_math_elec_rc_cutoff(double r, double c);
double cdb_math_elec_lc_resonance(double l, double c);
double cdb_math_elec_reactance_c(double f, double c);
double cdb_math_elec_reactance_l(double f, double l);

/* ---------------------------------------------------------------- Atmospheric & Gas */
double cdb_math_atmo_pressure_to_alt(double p_hpa, double p0_hpa);
double cdb_math_atmo_alt_to_sealevel_p(double p_hpa, double alt_m, double temp_c);
double cdb_math_atmo_dew_point(double temp_c, double rh_percent);
double cdb_math_atmo_vapor_pressure(double temp_c, double rh_percent);
double cdb_math_atmo_heat_index(double temp_c, double rh_percent);
double cdb_math_atmo_wind_chill(double temp_c, double wind_mps);
double cdb_math_atmo_air_density(double p_pa, double temp_c, double rh_percent);
double cdb_math_gas_ideal_pressure(double n_moles, double temp_k, double volume_m3);

/* ---------------------------------------------------------------- Fluid Dynamics & Thermal */
double cdb_math_fluid_dynamic_pressure(double density, double velocity);
double cdb_math_fluid_pitot_airspeed(double total_p_pa, double static_p_pa, double density);
double cdb_math_fluid_reynolds(double density, double velocity, double diameter, double viscosity);
double cdb_math_fluid_hydrostatic_pressure(double density, double depth_m);
double cdb_math_fluid_venturi_flow(double delta_p_pa, double density, double cd, double area1, double area2);
double cdb_math_thermal_junction_temp(double ambient_c, double power_watts, double theta_ja);
double cdb_math_thermal_radiation_power(double emissivity, double area_m2, double temp_k);

#endif /* CDB_MATH_H */
