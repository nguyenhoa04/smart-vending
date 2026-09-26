#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hx711.h"

#define TEST_SCK GPIO_NUM_25
#define CHANNEL_COUNT 2

static const gpio_num_t TEST_DOUTS[CHANNEL_COUNT] = {
    GPIO_NUM_34,
    GPIO_NUM_35,
};

typedef struct {
    uint32_t raw24[CHANNEL_COUNT];
    bool ready[CHANNEL_COUNT];
    bool sck_high;
    uint8_t completed_clocks;
    uint32_t rising_edges;
    int64_t fake_time_us;
    uint32_t gpio_config_calls;
} fake_hardware_t;

static fake_hardware_t fake;
static unsigned tests_run;
static unsigned tests_failed;

static int channel_for_pin(gpio_num_t pin) {
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (TEST_DOUTS[i] == pin) {
            return i;
        }
    }
    return -1;
}

static void fake_reset(void) {
    memset(&fake, 0, sizeof(fake));
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        fake.ready[i] = true;
    }
}

/* ESP-IDF GPIO/time functions replaced by deterministic host-side fakes. */
int gpio_config(const gpio_config_t *config) {
    if (config == NULL || config->pin_bit_mask == 0) {
        return -1;
    }
    ++fake.gpio_config_calls;
    return ESP_OK;
}

int gpio_set_level(gpio_num_t pin, uint32_t level) {
    if (pin != TEST_SCK) {
        return -1;
    }

    if (level != 0 && !fake.sck_high) {
        fake.sck_high = true;
        ++fake.rising_edges;
    } else if (level == 0 && fake.sck_high) {
        fake.sck_high = false;
        ++fake.completed_clocks;
    }
    return 0;
}

int gpio_get_level(gpio_num_t pin) {
    int channel = channel_for_pin(pin);
    if (channel < 0) {
        return 1;
    }

    /* Before the first SCK edge, DOUT LOW means conversion ready. */
    if (!fake.sck_high) {
        return fake.ready[channel] ? 0 : 1;
    }

    /* During clocks 1..24, present MSB first for this channel. */
    if (fake.completed_clocks < 24) {
        uint8_t shift = (uint8_t)(23 - fake.completed_clocks);
        return (int)((fake.raw24[channel] >> shift) & 1U);
    }
    return 0;
}

void esp_rom_delay_us(uint32_t us) {
    fake.fake_time_us += us;
}

int64_t esp_timer_get_time(void) {
    /* Progress time even while the driver is polling an unready converter. */
    fake.fake_time_us += 100;
    return fake.fake_time_us;
}

void vTaskDelay(uint32_t ticks) {
    fake.fake_time_us += (int64_t)ticks * 1000;
}

static void fail_at(const char *test, int line, const char *message) {
    fprintf(stderr, "FAIL %-38s line %d: %s\n", test, line, message);
    ++tests_failed;
}

#define EXPECT_TRUE(test_name, expression)                                      \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fail_at((test_name), __LINE__, #expression);                         \
            return;                                                              \
        }                                                                        \
    } while (0)

#define EXPECT_EQ_I32(test_name, expected, actual)                               \
    do {                                                                         \
        int32_t expected_value = (int32_t)(expected);                            \
        int32_t actual_value = (int32_t)(actual);                                \
        if (expected_value != actual_value) {                                    \
            char message[128];                                                   \
            snprintf(                                                            \
                message,                                                         \
                sizeof(message),                                                 \
                "expected %ld, got %ld",                                        \
                (long)expected_value,                                            \
                (long)actual_value                                               \
            );                                                                   \
            fail_at((test_name), __LINE__, message);                             \
            return;                                                              \
        }                                                                        \
    } while (0)

#define EXPECT_NEAR_F(test_name, expected, actual, tolerance)                    \
    do {                                                                         \
        float expected_value = (float)(expected);                                \
        float actual_value = (float)(actual);                                    \
        if (fabsf(expected_value - actual_value) > (float)(tolerance)) {         \
            char message[128];                                                   \
            snprintf(                                                            \
                message,                                                         \
                sizeof(message),                                                 \
                "expected %.6f, got %.6f",                                      \
                expected_value,                                                  \
                actual_value                                                     \
            );                                                                   \
            fail_at((test_name), __LINE__, message);                             \
            return;                                                              \
        }                                                                        \
    } while (0)

static void test_reads_two_parallel_values(void) {
    const char *name = "two parallel positive values";
    ++tests_run;
    fake_reset();
    fake.raw24[0] = 0x00123456U;
    fake.raw24[1] = 0x00000000U;

    int32_t actual[CHANNEL_COUNT] = {0};
    EXPECT_TRUE(
        name,
        hx711_read_multi_raw_timeout(
            TEST_SCK,
            TEST_DOUTS,
            CHANNEL_COUNT,
            actual,
            200
        )
    );
    EXPECT_EQ_I32(name, 0x00123456, actual[0]);
    EXPECT_EQ_I32(name, 0, actual[1]);
    EXPECT_EQ_I32(name, 25, fake.rising_edges);
}

static void test_sign_extends_negative_values(void) {
    const char *name = "signed 24-bit conversion";
    ++tests_run;
    fake_reset();
    fake.raw24[0] = 0x00FFFFFFU; /* -1 */
    fake.raw24[1] = 0x00800000U; /* minimum signed 24-bit */

    int32_t actual[CHANNEL_COUNT] = {0};
    EXPECT_TRUE(
        name,
        hx711_read_multi_raw_timeout(
            TEST_SCK,
            TEST_DOUTS,
            CHANNEL_COUNT,
            actual,
            200
        )
    );
    EXPECT_EQ_I32(name, -1, actual[0]);
    EXPECT_EQ_I32(name, INT32_C(-8388608), actual[1]);
    EXPECT_EQ_I32(name, 25, fake.rising_edges);
}

static void test_channels_do_not_mix_bits(void) {
    const char *name = "channel data stays independent";
    ++tests_run;
    fake_reset();
    fake.raw24[0] = 0x00AAAAAAU;
    fake.raw24[1] = 0x00555555U;

    int32_t actual[CHANNEL_COUNT] = {0};
    EXPECT_TRUE(
        name,
        hx711_read_multi_raw_timeout(
            TEST_SCK,
            TEST_DOUTS,
            CHANNEL_COUNT,
            actual,
            200
        )
    );
    EXPECT_EQ_I32(name, (int32_t)0xFFAAAAAAU, actual[0]);
    EXPECT_EQ_I32(name, 0x00555555, actual[1]);
}

static void test_waits_for_every_dout_and_times_out(void) {
    const char *name = "timeout when one DOUT stays high";
    ++tests_run;
    fake_reset();
    fake.ready[1] = false;

    int32_t actual[CHANNEL_COUNT] = {111, 222};
    EXPECT_TRUE(
        name,
        !hx711_read_multi_raw_timeout(
            TEST_SCK,
            TEST_DOUTS,
            CHANNEL_COUNT,
            actual,
            5
        )
    );
    EXPECT_EQ_I32(name, 0, fake.rising_edges);
    EXPECT_EQ_I32(name, 111, actual[0]);
    EXPECT_EQ_I32(name, 222, actual[1]);
}

static void test_calibration_math(void) {
    const char *name = "offset and scale calibration";
    ++tests_run;
    hx711_config_t config = {
        .id = 1,
        .dout_pin = GPIO_NUM_34,
        .sck_pin = GPIO_NUM_25,
        .offset = -186477,
        .scale = 32.0f,
    };

    EXPECT_NEAR_F(
        name,
        500.0f,
        hx711_calibrate_raw(&config, -170477),
        0.001f
    );
    EXPECT_NEAR_F(
        name,
        -500.0f,
        hx711_calibrate_raw(&config, -202477),
        0.001f
    );

    config.scale = 0.0f;
    EXPECT_NEAR_F(name, 0.0f, hx711_calibrate_raw(&config, 123), 0.001f);
}

static void test_shared_gpio_initialization(void) {
    const char *name = "shared GPIO initialization";
    ++tests_run;
    fake_reset();

    EXPECT_TRUE(
        name,
        hx711_init_shared(TEST_SCK, TEST_DOUTS, CHANNEL_COUNT)
    );
    EXPECT_EQ_I32(name, 2, fake.gpio_config_calls);
    EXPECT_TRUE(
        name,
        !hx711_init_shared(GPIO_NUM_34, TEST_DOUTS, CHANNEL_COUNT)
    );
}

int main(void) {
    test_reads_two_parallel_values();
    test_sign_extends_negative_values();
    test_channels_do_not_mix_bits();
    test_waits_for_every_dout_and_times_out();
    test_calibration_math();
    test_shared_gpio_initialization();

    if (tests_failed == 0) {
        printf("PASS: %u/%u HX711 unit tests passed.\n", tests_run, tests_run);
        return EXIT_SUCCESS;
    }

    fprintf(
        stderr,
        "FAILED: %u of %u HX711 unit tests failed.\n",
        tests_failed,
        tests_run
    );
    return EXIT_FAILURE;
}
