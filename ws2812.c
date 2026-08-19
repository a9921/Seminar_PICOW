#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

#define IS_RGBW false
#define NUM_PIXELS 150

#ifdef PICO_DEFAULT_WS2812_PIN
#define WS2812_PIN PICO_DEFAULT_WS2812_PIN
#else

#define WS2812_PIN 2
#endif
#undef WS2812_PIN
#define WS2812_PIN 16

#define TRIG_PIN 3
#define ECHO_PIN 2
#define BRIGHT 40

// Check the pin is compatible with the platform
#if WS2812_PIN >= NUM_BANK0_GPIOS
#error Attempting to use a pin>=32 on a platform that does not support it
#endif

static inline void put_pixel(PIO pio, uint sm, uint32_t pixel_grb)
{
    pio_sm_put_blocking(pio, sm, pixel_grb << 8u);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b)
{
    return  ((uint32_t)(r) << 8) |
            ((uint32_t)(g) << 16) |
            (uint32_t)(b);
}

static inline uint32_t urgbw_u32(uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    return ((uint32_t)(r) << 8) |
            ((uint32_t)(g) << 16) |
            ((uint32_t)(w) << 24) |
            (uint32_t)(b);
}

void pattern_snakes(PIO pio, uint sm, uint len, uint t)
{
    for (uint i = 0; i < len; ++i)
    {
        uint x = (i + (t >> 1)) % 64;
        if (x < 10)
            put_pixel(pio, sm, urgb_u32(0xff, 0, 0));
        else if (x >= 15 && x < 25)
            put_pixel(pio, sm, urgb_u32(0, 0xff, 0));
        else if (x >= 30 && x < 40)
            put_pixel(pio, sm, urgb_u32(0, 0, 0xff));
        else
            put_pixel(pio, sm, 0);
    }
}

void pattern_random(PIO pio, uint sm, uint len, uint t)
{
    if (t % 8)
        return;
    for (uint i = 0; i < len; ++i)
        put_pixel(pio, sm, rand());
}

void pattern_sparkle(PIO pio, uint sm, uint len, uint t)
{
    if (t % 8)
        return;
    for (uint i = 0; i < len; ++i)
        put_pixel(pio, sm, rand() % 16 ? 0 : 0xffffffff);
}

void pattern_greys(PIO pio, uint sm, uint len, uint t)
{
    uint max = 100; // let's not draw too much current!
    t %= max;
    for (uint i = 0; i < len; ++i)
    {
        put_pixel(pio, sm, t * 0x10101);
        if (++t >= max)
            t = 0;
    }
}

typedef void (*pattern)(PIO pio, uint sm, uint len, uint t);
const struct
{
    pattern pat;
    const char *name;
} pattern_table[] = {
    {pattern_snakes, "Snakes!"},
    {pattern_random, "Random data"},
    {pattern_sparkle, "Sparkles"},
    {pattern_greys, "Greys"},
};

float measure_distance_cm(void)
{
    // 1. 送出 10us 的觸發脈衝
    gpio_put(TRIG_PIN, 0);
    sleep_us(2);
    gpio_put(TRIG_PIN, 1);
    sleep_us(10);
    gpio_put(TRIG_PIN, 0);

    // 2. 等 Echo 轉為高電位（最多等 50ms，避免程式卡死）
    absolute_time_t deadline = make_timeout_time_ms(50);
    while (gpio_get(ECHO_PIN) == 0)
    {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
            return -1.0f;
    }
    absolute_time_t t_start = get_absolute_time();

    // 3. 等 Echo 轉回低電位
    deadline = make_timeout_time_ms(50);
    while (gpio_get(ECHO_PIN) == 1)
    {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
            return -1.0f;
    }
    absolute_time_t t_end = get_absolute_time();

    // 4. 換算：來回時間 x 音速(0.0343 cm/us) / 2
    int64_t pulse_us = absolute_time_diff_us(t_start, t_end);
    return (float)pulse_us * 0.0343f / 2.0f;
}

static void fill_all(PIO pio, uint sm, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < NUM_PIXELS; i++)
    {
        put_pixel(pio, sm, urgb_u32(r, g, b));
    }
}

int main()
{
    // set_sys_clock_48();
    stdio_init_all();
    printf("WS2812 Smoke Test, using pin %d\n", WS2812_PIN);

    // todo get free sm
    PIO pio;
    uint sm;
    uint offset;

    // This will find a free pio and state machine for our program and load it for us
    // We use pio_claim_free_sm_and_add_program_for_gpio_range (for_gpio_range variant)
    // so we will get a PIO instance suitable for addressing gpios >= 32 if needed and supported by the hardware
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_program, &pio, &sm, &offset, WS2812_PIN, 1, true);
    hard_assert(success);

    ws2812_program_init(pio, sm, offset, WS2812_PIN, 800000, IS_RGBW);

    int t = 0;

    stdio_init_all();

    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);
    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);

    while (true)
    {
        float d = measure_distance_cm();

        if (d < 0.0f)
        {
            printf("no echo\n");
            fill_all(pio, sm, 0, 0, BRIGHT); // 藍燈：量測失敗
        }
        else if (d > 100.0f)
        {
            printf("%.1f cm -> 綠\n", d);
            fill_all(pio, sm, 0, BRIGHT, 0); // 綠燈：正常
        }
        else if (d >= 30.0f)
        {
            printf("%.1f cm -> 黃\n", d);
            fill_all(pio, sm, BRIGHT, BRIGHT / 2, 0); // 黃燈：警戒
        }
        else
        {
            printf("%.1f cm -> 紅\n", d);
            fill_all(pio, sm, BRIGHT, 0, 0); // 紅燈：警報
        }

        sleep_ms(500);
    }

    // This will free resources and unload our program
    pio_remove_program_and_unclaim_sm(&ws2812_program, pio, sm, offset);
}
