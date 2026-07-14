// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Host-side unit tests for the EQ profile system (App/Src/eq_profile.c),
 * focused on the host-facing attack surface: coefficient validation,
 * profile slot management, and the biquad processing path.
 *
 * Compiled against tests/stubs/ so no hardware is touched; eq_profile_init()
 * (which reads memory-mapped flash) is intentionally NOT called — the RAM
 * store starts zeroed, which is exactly the "empty store" state.
 */

#include "eq_profile.h"
#include "test_util.h"
#include <math.h>
#include <string.h>

#define BUF_SAMPLES 64

// A well-formed pass-through biquad (b0=1, everything else 0)
static eq_profile_t make_passthrough_profile(void) {
    eq_profile_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.name, "test");
    p.filter_count = 1;
    p.filters[0].b0 = 1.0f;
    p.filters[0].type = FILTER_BELL;
    p.filters[0].enabled = 1;
    p.filters[0].freq = 1000.0f;
    p.filters[0].gain = 0.0f;
    p.filters[0].q = 0.707f;
    return p;
}

static void test_valid_profile_accepted(void) {
    eq_profile_t p = make_passthrough_profile();
    CHECK(eq_profile_set(0, &p));
    CHECK(eq_profile_get(0) != NULL);
    CHECK_EQ_I32(eq_profile_count(), 1);
    CHECK(eq_profile_delete(0));
    CHECK(eq_profile_get(0) == NULL);
    CHECK_EQ_I32(eq_profile_count(), 0);
}

static void test_nan_and_inf_coefficients_rejected(void) {
    eq_profile_t p = make_passthrough_profile();
    p.filters[0].b0 = NAN;
    CHECK(!eq_profile_set(0, &p));

    p = make_passthrough_profile();
    p.filters[0].a1 = INFINITY;
    CHECK(!eq_profile_set(0, &p));

    p = make_passthrough_profile();
    p.filters[0].gain = -INFINITY;
    CHECK(!eq_profile_set(0, &p));

    CHECK(eq_profile_get(0) == NULL); // nothing was stored
}

static void test_unstable_filters_rejected(void) {
    // Pole outside the unit circle: |a2| >= 1
    eq_profile_t p = make_passthrough_profile();
    p.filters[0].a2 = 1.5f;
    CHECK(!eq_profile_set(0, &p));

    p = make_passthrough_profile();
    p.filters[0].a2 = -1.0f;
    CHECK(!eq_profile_set(0, &p));

    // Stability triangle violation: |a1| >= 1 + a2
    p = make_passthrough_profile();
    p.filters[0].a2 = 0.9f;
    p.filters[0].a1 = 2.6f; // limit is 1.9
    CHECK(!eq_profile_set(0, &p));

    // Just inside the triangle: accepted
    p = make_passthrough_profile();
    p.filters[0].a2 = 0.9f;
    p.filters[0].a1 = 1.8f;
    CHECK(eq_profile_set(0, &p));
    CHECK(eq_profile_delete(0));
}

static void test_disabled_filter_with_garbage_is_bypassed(void) {
    // A disabled filter never runs, so its coefficients may be anything
    eq_profile_t p = make_passthrough_profile();
    p.filter_count = 2;
    p.filters[1].b0 = NAN;
    p.filters[1].a2 = 99.0f;
    p.filters[1].enabled = 0;
    p.filters[1].type = FILTER_BELL;
    CHECK(eq_profile_set(0, &p));
    CHECK(eq_profile_delete(0));
}

static void test_invalid_slot_and_null_rejected(void) {
    eq_profile_t p = make_passthrough_profile();
    CHECK(!eq_profile_set(EQ_MAX_PROFILES, &p));
    CHECK(!eq_profile_set(0, NULL));
    CHECK(!eq_profile_delete(EQ_MAX_PROFILES));
    CHECK(eq_profile_get(EQ_MAX_PROFILES) == NULL);
}

static void test_active_profile_lifecycle(void) {
    eq_profile_t p = make_passthrough_profile();
    CHECK(eq_profile_set(2, &p));

    eq_profile_set_active(2);
    CHECK_EQ_I32(eq_profile_get_active(), 2);
    CHECK(strcmp(eq_profile_get_active_name(), "test") == 0);

    // Activating an empty slot is ignored
    eq_profile_set_active(5);
    CHECK_EQ_I32(eq_profile_get_active(), 2);

    // Deleting the active profile deactivates it
    CHECK(eq_profile_delete(2));
    CHECK_EQ_I32(eq_profile_get_active(), EQ_PROFILE_OFF);
    CHECK(strcmp(eq_profile_get_active_name(), "OFF") == 0);
}

static void test_passthrough_processing_is_exact(void) {
    // b0=1 biquad at unity volume: 24-bit samples survive the float
    // round-trip exactly (float32 mantissa covers 24-bit integers)
    eq_profile_t p = make_passthrough_profile();
    CHECK(eq_profile_set(0, &p));
    eq_profile_set_active(0);
    eq_profile_reset_state();

    int32_t buf[BUF_SAMPLES], orig[BUF_SAMPLES];
    for (int i = 0; i < BUF_SAMPLES; i++)
        buf[i] = (i * 262144) - 8388608; // spread across 24-bit range
    memcpy(orig, buf, sizeof(buf));

    eq_profile_process(buf, BUF_SAMPLES, 65536);
    for (int i = 0; i < BUF_SAMPLES; i++)
        CHECK_EQ_I32(buf[i], orig[i]);

    CHECK(eq_profile_delete(0));
    eq_profile_set_active(EQ_PROFILE_OFF);
}

static void test_processing_applies_volume(void) {
    eq_profile_t p = make_passthrough_profile();
    CHECK(eq_profile_set(0, &p));
    eq_profile_set_active(0);
    eq_profile_reset_state();

    int32_t buf[BUF_SAMPLES];
    for (int i = 0; i < BUF_SAMPLES; i++)
        buf[i] = 1000000;

    eq_profile_process(buf, BUF_SAMPLES, 32768); // half volume
    for (int i = 0; i < BUF_SAMPLES; i++)
        CHECK_EQ_I32(buf[i], 500000);

    CHECK(eq_profile_delete(0));
    eq_profile_set_active(EQ_PROFILE_OFF);
}

static void test_off_profile_leaves_buffer_untouched(void) {
    eq_profile_set_active(EQ_PROFILE_OFF);

    int32_t buf[BUF_SAMPLES], orig[BUF_SAMPLES];
    for (int i = 0; i < BUF_SAMPLES; i++)
        buf[i] = i * 1000 - 32000;
    memcpy(orig, buf, sizeof(buf));

    eq_profile_process(buf, BUF_SAMPLES, 32768); // volume ignored when OFF
    CHECK(memcmp(buf, orig, sizeof(buf)) == 0);
}

static void test_filter_count_clamped(void) {
    eq_profile_t p = make_passthrough_profile();
    p.filter_count = 200; // out of range; sane filters only in slot 0
    CHECK(eq_profile_set(0, &p));
    const eq_profile_t *stored = eq_profile_get(0);
    CHECK(stored != NULL);
    if (stored != NULL)
        CHECK(stored->filter_count <= EQ_MAX_FILTERS);
    CHECK(eq_profile_delete(0));
}

int main(void) {
    test_valid_profile_accepted();
    test_nan_and_inf_coefficients_rejected();
    test_unstable_filters_rejected();
    test_disabled_filter_with_garbage_is_bypassed();
    test_invalid_slot_and_null_rejected();
    test_active_profile_lifecycle();
    test_passthrough_processing_is_exact();
    test_processing_applies_volume();
    test_off_profile_leaves_buffer_untouched();
    test_filter_count_clamped();
    return test_summary("eq_profile");
}
