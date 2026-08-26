#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"
#include "hardware/uart.h"

//硬體設定
#define IS_RGBW false
#define NUM_PIXELS 8
#define WS2812_PIN 15

#define TRIG_PIN 3
#define ECHO_PIN 2
#define BRIGHT 40
#define UART_ID uart0
#define BAUD_RATE 115200
#define TX_PIN 0
#define RX_PIN 1

//距離設置
#define DIST_WARN 100.0f 
#define DIST_ALERT 30.0f 
#define SAMPLES 5
#define SAME_STATE 3

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

// 測量值存進緩衝區
static void push_sample(float d)
{
    sample_buf[sample_idx] = d;
    sample_idx = (sample_idx + 1) % SAMPLES;
    if(sample_idx == 0)
        buf_filled = true;
}

//取出中位數
static float get_median(void)
{
    float tmp[SAMPLES];
    int n = 0;
    int limit = buf_filled ? SAMPLES : sample_idx;

    for(int i = 0; i < limit; i++)
    {
        if(sample_buf[i] > 0.0f)
            tmp[n++] = sample_buf[i];
    }

    if(n == 0)
        return -1.0f;
    
    // 測量值排序
    for(int i = 0; i < n ; i++)
    {
        float key = tmp[i];
        int j = i -1;
        while (j >= 0 && tmp[j] > key)
        {
            tmp[j+1] = tmp[j];
            j--;
        }
        tmp[j+1] = key;
    }
    return tmp[n/2];
}

/* 狀態判斷 */ 
// 距離判斷
static sys_state_t classify_distance(float d)
{
    if(d < 0.0f)
        return STATE_ERROR;
    if(d > DIST_WARN)
        return STATE_NORMAL;
    if(d > DIST_ALERT)
        return STATE_WARN;
    return STATE_ALERT;
}

// 連續狀態相同
static sys_state_t debounce(sys_state_t now)
{
    static sys_state_t stable = STATE_NORMAL;
    static sys_state_t temp_state = STATE_NORMAL;
    static int count = 0;

    if(now == temp_state)
    {
        if(count < SAME_STATE)
            count++;
    }
    else
    {
        temp_state = now;
        count = 1;
    }

    if(count >= SAME_STATE)
    {
        stable = temp_state;
    }
    return stable;
}

// 狀態轉成燈號
static void render_state(PIO pio, uint sm, sys_state_t st, bool blink_on)
{
    switch (st)
    {
    case STATE_ERROR:
        fill_all(pio, sm, 0, 0, BRIGHT);   // 藍燈：量測失敗
        break;
    case STATE_NORMAL:
        fill_all(pio, sm, 0, BRIGHT, 0);    // 綠燈：正常;
        break;
    case STATE_WARN:
        fill_all(pio, sm, BRIGHT * 3 / 5, BRIGHT * 4 / 5, 0);   // 黃燈：警戒
        break;
    case STATE_ALERT:
        fill_all(pio, sm, BRIGHT, 0, 0);    // 紅燈：警報
        break;
    case STATE_ANOMALY:
        if (blink_on)
            fill_all(pio, sm, BRIGHT, 0, 0);    // 閃紅燈 : 門開可無人
        else
            fill_all(pio, sm, 0, 0, 0);     // 關燈 : 修理中
        break;
    }
}


/* 主程式 */
int main()
{
    // set_sys_clock_48();
    stdio_init_all();
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(RX_PIN, GPIO_FUNC_UART);

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

    uint32_t tick = 0;

    // 主迴圈: 測量->判斷->顯示,每0.1秒一次
    while (true)
    {
        push_sample(measure_distance_cm());     //測量距離
        float d = get_median();

        sys_state_t state = debounce(classify_distance(d));    //狀態分類

        bool blink_on = ((tick / BLINK_TICKS) % 2) == 0;    //閃爍

        render_state(pio, sm, state, blink_on);     //狀態顯示

        if(tick % PRINT_TICKS == 0)     //狀態文字輸出
        {
            char msg[64];
            int dist_mm = -1;
            if(d < 0.0f)
            {
                printf("no echo     -> %s\n", state_name(state));
                dist_mm = -1;
            }
            else
            {
                printf("%6.1f cm    -> %s\n", d, state_name(state));
                dist_mm = (int)(d * 10.0f);
            }
            snprintf(msg, sizeof(msg), "{\"dist_mm\": %d, \"state\": %d}\r\n", dist_mm, state);
            uart_puts(UART_ID, msg);
        }
        tick++;    
        sleep_ms(TICK_MS);
    }
}
