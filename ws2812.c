#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"

//硬體設定
#define IS_RGBW false
#define NUM_PIXELS 8
#define WS2812_PIN 16

#define TRIG_PIN 3
#define ECHO_PIN 2
#define BRIGHT 40

//距離設置
#define DIST_WARN 100.0f 
#define DIST_ALERT 30.0f 
#define SAMPLES 5
#define CONFIRM_COUNT 3

//時間設置
#define TICK_MS 100
#define BLINK_TICKS 4
#define PRINT_TICKS 5

// Check the pin is compatible with the platform
#if WS2812_PIN >= NUM_BANK0_GPIOS
#error Attempting to use a pin>=32 on a platform that does not support it
#endif

/* 系統顯示顏色設置 */
typedef enum
{
    STATE_ERROR = 0,  //測量失敗    藍
    STATE_NORMAL,     //安全        綠
    STATE_WARN,       //有人在附近  黃
    STATE_ALERT,      //靠近機櫃    紅燈恆亮
    STATE_ANOMALY     //門開卻無人  紅燈閃爍
}sys_state_t;

static const char *state_name(sys_state_t st)
{
    switch (st)
    {
    case STATE_ERROR:
        return "ERROR (blue)";
    case STATE_NORMAL:
        return "NORMAL (green)";
    case STATE_WARN:
        return "WARN (yellow)";
    case STATE_ALERT:
        return "ALERT (red)";
    case STATE_ANOMALY:
        return "ANOMALY (red blink)";
    }
    return "UNKNOW";
}

/* WS2812B 硬體設置 */

// 把24bit的顏色放進PIO佇列
static inline void put_pixel(PIO pio, uint sm, uint32_t pixel_grb)
{
    pio_sm_put_blocking(pio, sm, pixel_grb << 8u);
}

// (R,G,B)命令 改成 (G-R-B) 晶片指定
static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b)
{
    return  ((uint32_t)(r) << 8) |
            ((uint32_t)(g) << 16) |
            (uint32_t)(b);
}

// 燈條顯示同一顏色
static void fill_all(PIO pio, uint sm, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < NUM_PIXELS; i++)
    {
        put_pixel(pio, sm, urgb_u32(r, g, b));
    }
}

/* 超音波控制 */

// 觸發測距，回傳距離(公分);逾時或無回波傳-1 
float measure_distance_cm(void)
{

    // 0. 確認回歸原狀
    absolute_time_t deadline = make_timeout_time_ms(10);
    while (gpio_get(ECHO_PIN) == 1)
    {
        /* code */
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
        {
            /* code */
            return -1.0f;
        }
    }
    

    // 1. 送出 10us 的觸發脈衝
    gpio_put(TRIG_PIN, 0);
    sleep_us(2);
    gpio_put(TRIG_PIN, 1);
    sleep_us(10);
    gpio_put(TRIG_PIN, 0);

    // 2. 等 Echo 轉為高電位（最多等 50ms，避免程式卡死）
    deadline = make_timeout_time_ms(50);
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

/* 測量緩衝 */
static float sample_buf[SAMPLES];
static int sample_idx = 0;
static bool buf_filled = false;


static void push_sample(float d)
{
    sample_buf[sample_idx] = d;
    sample_idx = (sample_idx + 1) % SAMPLES;
    if(sample_idx == 0)
        buf_filled = true;
}


/* 主程式 */
int main()
{
    // set_sys_clock_48();
    stdio_init_all();
    sleep_ms(2000);
    printf("超音波距離顯示器, using pin %d\n", WS2812_PIN);
    
    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);
    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);

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

    // 主迴圈: 測量->判斷->顯示,每0.5秒一次
    while (true)
    {
        float d = measure_distance_cm();

        if (d < 0.0f)
        {
            printf("no echo\n");
            fill_all(pio, sm, 0, 0, BRIGHT); // 藍燈：量測失敗
        }
        else if (d > DIST_WARN)
        {
            printf("%.1f cm -> 綠\n", d);
            fill_all(pio, sm, 0, BRIGHT, 0); // 綠燈：正常
        }
        else if (d >= DIST_ALERT)
        {
            printf("%.1f cm -> 黃\n", d);
            fill_all(pio, sm, BRIGHT * 2 / 5, BRIGHT, 0); // 黃燈：警戒
        }
        else
        {
            printf("%.1f cm -> 紅\n", d);
            fill_all(pio, sm, BRIGHT, 0, 0); // 紅燈：警報
        }

        sleep_ms(500);
    }
}
