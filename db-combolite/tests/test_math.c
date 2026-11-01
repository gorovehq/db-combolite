/* Unit tests for CMBQL mathematics, data science, DSP, navigation and linear algebra. */
#include "unity.h"
#include "cdb_math.h"
#include "cdb_eval.h"
#include "cdb_query.h"
#include "combolite/cdb_value.h"

#include <math.h>
#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

static size_t make_f64_lit(uint8_t *buf, size_t cap, double val)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_BIG, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_f64(&w, val));
    return cdb_value_writer_len(&w);
}

static void test_trigonometry(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, (float)cdb_math_sin(0.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, (float)cdb_math_sin(1.57079632679));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, (float)cdb_math_cos(0.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, (float)cdb_math_cos(1.57079632679));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.785398f, (float)cdb_math_atan2(1.0, 1.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 180.0f, (float)cdb_math_degrees(3.14159265359));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.14159265359f, (float)cdb_math_radians(180.0));
}

static void test_geometry_and_navigation(void)
{
    /* Distance NYC (40.7128, -74.0060) to London (51.5074, -0.1278) ~ 5570 km */
    double dist = cdb_math_haversine(40.7128, -74.0060, 51.5074, -0.1278);
    TEST_ASSERT_FLOAT_WITHIN(50000.0f, 5570000.0f, (float)dist);

    /* Initial bearing NYC to London ~ 51 degrees */
    double brg = cdb_math_bearing(40.7128, -74.0060, 51.5074, -0.1278);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 51.0f, (float)brg);

    /* Destination: start at (0, 0), move east 111.32 km (bearing 90) -> (0, 1.0) */
    {
        double dlat = 0.0, dlon = 0.0;
        cdb_math_destination(0.0, 0.0, 90.0, 111319.5, &dlat, &dlon);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, (float)dlat);
        TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, (float)dlon);
    }

    /* Cross-track distance: point on equator moving east, test point at 1 deg north */
    {
        double xtd = cdb_math_cross_track_distance(0.0, 0.0, 0.0, 10.0, 1.0, 5.0);
        TEST_ASSERT_FLOAT_WITHIN(5000.0f, 111319.5f, (float)fabs(xtd));
    }

    /* Geofencing: Circle */
    TEST_ASSERT_TRUE(cdb_math_point_in_circle(40.7128, -74.0060, 40.7128, -74.0060, 100.0));
    TEST_ASSERT_FALSE(cdb_math_point_in_circle(40.7128, -74.0060, 41.0, -74.0, 1000.0));

    /* Geofencing: Polygon (square around 0,0) */
    {
        double plats[4] = {-1.0, 1.0, 1.0, -1.0};
        double plons[4] = {-1.0, -1.0, 1.0, 1.0};
        TEST_ASSERT_TRUE(cdb_math_point_in_polygon(0.0, 0.0, plats, plons, 4));
        TEST_ASSERT_FALSE(cdb_math_point_in_polygon(2.0, 2.0, plats, plons, 4));
    }

    /* Web Mercator projection round-trip */
    {
        double lat = 35.6762, lon = 139.6503; /* Tokyo */
        double x = cdb_math_wgs84_to_mercator_x(lon);
        double y = cdb_math_wgs84_to_mercator_y(lat);
        double rlon = cdb_math_mercator_to_wgs84_lon(x);
        double rlat = cdb_math_mercator_to_wgs84_lat(y);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, (float)lat, (float)rlat);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, (float)lon, (float)rlon);
    }

    /* Geohash */
    {
        char hash[16];
        double dlat = 0.0, dlon = 0.0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_geohash_encode(37.7749, -122.4194, 6, hash, sizeof(hash)));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_geohash_decode(hash, 6, &dlat, &dlon));
        TEST_ASSERT_FLOAT_WITHIN(0.1f, 37.7749f, (float)dlat);
        TEST_ASSERT_FLOAT_WITHIN(0.1f, -122.4194f, (float)dlon);
    }
}

static void test_algebra_activations_and_calculus(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, (float)cdb_math_sqrt(9.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 8.0f, (float)cdb_math_pow(2.0, 3.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, (float)cdb_math_sigmoid(0.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, (float)cdb_math_sigmoid(20.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, (float)cdb_math_sigmoid(-20.0));

    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, (float)cdb_math_relu(5.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, (float)cdb_math_relu(-5.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8427f, (float)cdb_math_erf(1.0));

    /* Calculus */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 10.0f, (float)cdb_math_diff(25.0, 15.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, (float)cdb_math_derivative(10.0, 2.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 20.0f, (float)cdb_math_trapezoid(3.0, 5.0, 5.0));
}

static void test_sdr_and_signal(void)
{
    /* IQ magnitude & phase */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, (float)cdb_math_iq_magnitude(3.0, 4.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.927295f, (float)cdb_math_iq_phase(3.0, 4.0));

    /* FM Demodulation: rotating vector at constant frequency */
    {
        double i0 = 1.0, q0 = 0.0;
        double i1 = 0.0, q1 = 1.0; /* 90 degrees advance */
        double fm = cdb_math_iq_demod_fm(i1, q1, i0, q0);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.570796f, (float)fm);
    }

    /* dBFS */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, (float)cdb_math_dbfs(1.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -20.0f, (float)cdb_math_dbfs(0.01));

    /* FIR Filter */
    {
        float sig[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
        float taps[2] = {0.5f, 0.5f};
        float out[5];
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_fir_filter(sig, 5, taps, 2, out));
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, out[0]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.5f, out[1]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.5f, out[2]);
    }
}

static void test_fft_spectral(void)
{
    /* 8-point FFT of DC signal: [1, 1, 1, 1, 1, 1, 1, 1] */
    float r[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float im[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float wr[8], wi[8], mag[5];

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_fft(r, im, 8, 0));
    /* DC bin (bin 0) should have energy 8.0, all other bins 0 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 8.0f, r[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, r[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, r[2]);

    /* Inverse FFT restores original DC signal */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_fft(r, im, 8, 1));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, r[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, r[1]);

    /* Magnitude spectrum */
    {
        float sig[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_fft_magnitude(sig, 8, wr, wi, mag));
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 8.0f, mag[0]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, mag[1]);
    }
}

static void test_linear_algebra(void)
{
    float u[3] = {1.0f, 2.0f, 3.0f};
    float v[3] = {4.0f, 5.0f, 6.0f};
    float out = 0.0f;

    /* Dot product: 1*4 + 2*5 + 3*6 = 32 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_vec_dot(u, v, 3, &out));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 32.0f, out);

    /* Vector norm: sqrt(1+4+9) = sqrt(14) ~ 3.741657 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_vec_norm(u, 3, &out));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.741657f, out);

    /* Matrix 2x2 determinant: [1 2; 3 4] -> 1*4 - 2*3 = -2 */
    {
        float m2[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float det = 0.0f;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_mat_det(m2, 2, &det));
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, -2.0f, det);
    }

    /* Matrix multiplication 2x2: A = [1 2; 3 4], B = [2 0; 1 2] -> C = [4 4; 10 8] */
    {
        float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float b[4] = {2.0f, 0.0f, 1.0f, 2.0f};
        float c[4];
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_math_mat_mul(a, 2, 2, b, 2, c));
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 4.0f, c[0]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 4.0f, c[1]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 10.0f, c[2]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 8.0f, c[3]);
    }
}

static void test_call_expression_wire_eval(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[256];
    uint8_t v1[32], v2[32];
    size_t l1, l2;
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    /* Call CDB_FN_SQRT(16.0) -> 4.0 */
    l1 = make_f64_lit(v1, sizeof(v1), 16.0);
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_call(&w, CDB_FN_SQRT, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_F64, res.type);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 4.0f, (float)res.as.f64);

    /* Call CDB_FN_HYPOT(3.0, 4.0) -> 5.0 */
    l1 = make_f64_lit(v1, sizeof(v1), 3.0);
    l2 = make_f64_lit(v2, sizeof(v2), 4.0);
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_call(&w, CDB_FN_HYPOT, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_F64, res.type);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, (float)res.as.f64);
}

static void test_astronomy_and_telescope(void)
{
    /* Observer at telescope: 40.7128 N, -74.0060 W, 10m alt
     * Target aircraft: 40.7500 N, -73.9800 W, 3000m alt */
    double az = 0.0, alt = 0.0, range = 0.0;
    cdb_math_astro_topocentric(40.7128, -74.0060, 10.0,
                               40.7500, -73.9800, 3000.0,
                               &az, &alt, &range);
    TEST_ASSERT_TRUE(range > 4000.0 && range < 8000.0);
    TEST_ASSERT_TRUE(alt > 20.0 && alt < 50.0);
    TEST_ASSERT_TRUE(az > 10.0 && az < 50.0);

    /* Refraction for low elevation (e.g. 5 degrees) */
    {
        double refr = cdb_math_astro_refraction(5.0);
        TEST_ASSERT_TRUE(refr > 0.1 && refr < 0.25); /* around ~9.8 arcmin = ~0.16 deg */
    }

    /* Alt/Az to RA/Dec round-trip */
    {
        double ra = 0.0, dec = 0.0;
        double ralt = 0.0, raz = 0.0;
        int64_t ts = 1700000000000000000LL; /* fixed unix nanos */
        cdb_math_astro_altaz_to_radec(45.0, 180.0, 40.7128, -74.0060, ts, &ra, &dec);
        cdb_math_astro_radec_to_altaz(ra, dec, 40.7128, -74.0060, ts, &ralt, &raz);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, (float)ralt);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 180.0f, (float)raz);
    }
}

static void test_electronics(void)
{
    /* NTC Thermistor: R0 = 10k at 25C, Beta = 3950, measured R = 10k -> 25C */
    double t_ntc = cdb_math_elec_thermistor_ntc(10000.0, 10000.0, 3950.0, 25.0);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 25.0f, (float)t_ntc);

    /* ADC Volts: 2048 / 4096 * 3.3V = 1.65V */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.65f, (float)cdb_math_elec_adc_volts(2048.0, 4096.0, 3.3));

    /* Voltage divider: 10V across 1k and 1k -> 5V */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, (float)cdb_math_elec_voltage_divider(10.0, 1000.0, 1000.0));

    /* Parallel resistors: 100 // 100 = 50 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 50.0f, (float)cdb_math_elec_parallel_r(100.0, 100.0));

    /* dBm <-> Watts: 0 dBm = 0.001 W (1 mW) */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.001f, (float)cdb_math_elec_dbm_to_watts(0.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, (float)cdb_math_elec_watts_to_dbm(0.001));

    /* RC Cutoff: 1k Ohm, 1 uF -> ~159.15 Hz */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 159.15f, (float)cdb_math_elec_rc_cutoff(1000.0, 1e-6));
}

static void test_atmospheric_and_gas(void)
{
    /* Pressure altitude: 1013.25 hPa -> 0m */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, (float)cdb_math_atmo_pressure_to_alt(1013.25, 1013.25));

    /* Dew point: at 20C and 50% RH -> ~9.3C */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 9.3f, (float)cdb_math_atmo_dew_point(20.0, 50.0));

    /* Ideal gas pressure: 1 mole at 273.15K in 0.022414 m3 (~22.4L) -> ~101325 Pa */
    TEST_ASSERT_FLOAT_WITHIN(100.0f, 101325.0f, (float)cdb_math_gas_ideal_pressure(1.0, 273.15, 0.022414));

    /* Air density at sea level, 15C (101325 Pa) ~ 1.225 kg/m3 */
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.225f, (float)cdb_math_atmo_air_density(101325.0, 15.0, 0.0));
}

static void test_fluid_and_thermal(void)
{
    /* Dynamic pressure: rho = 1.225, v = 10 m/s -> 0.5 * 1.225 * 100 = 61.25 Pa */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 61.25f, (float)cdb_math_fluid_dynamic_pressure(1.225, 10.0));

    /* Pitot airspeed: delta_p = 61.25 Pa -> v = 10 m/s */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, (float)cdb_math_fluid_pitot_airspeed(101386.25, 101325.0, 1.225));

    /* Hydrostatic pressure: 1000 kg/m3 water at 10m depth -> ~98066.5 Pa */
    TEST_ASSERT_FLOAT_WITHIN(10.0f, 98066.5f, (float)cdb_math_fluid_hydrostatic_pressure(1000.0, 10.0));

    /* Junction temperature: 25C ambient + 2W * 40 C/W theta_ja -> 105C */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 105.0f, (float)cdb_math_thermal_junction_temp(25.0, 2.0, 40.0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trigonometry);
    RUN_TEST(test_geometry_and_navigation);
    RUN_TEST(test_algebra_activations_and_calculus);
    RUN_TEST(test_sdr_and_signal);
    RUN_TEST(test_fft_spectral);
    RUN_TEST(test_linear_algebra);
    RUN_TEST(test_call_expression_wire_eval);
    RUN_TEST(test_astronomy_and_telescope);
    RUN_TEST(test_electronics);
    RUN_TEST(test_atmospheric_and_gas);
    RUN_TEST(test_fluid_and_thermal);
    return UNITY_END();
}
