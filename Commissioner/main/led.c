#include "led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// --- Pin map (change to match your wiring) ---------------------------------
// Both LEDs are external (individual) LEDs on broken-out XIAO ESP32-C6 pads,
// each wired to GND through a resistor (active-HIGH). The onboard user LED is
// on GPIO15, which is NOT broken out to a pad on the XIAO C6, so it isn't used.
//   Commissioner LED -> GPIO21 (silk "D3")
//   System LED       -> GPIO2  (silk "D2")
// Avoid the C6 strapping pins (4,5,8,9,15) and the UART-to-C3 pads (16/17 = D6/D7).
#define LED_COMM_GPIO        21
#define LED_COMM_ACTIVE_LOW   0
#define LED_SYS_GPIO          2
#define LED_SYS_ACTIVE_LOW    0

#define LED_TICK_MS          25   // render cadence (~40 Hz)

static volatile led_comm_t s_comm  = LED_COMM_DISABLED;
static volatile led_role_t s_role  = LED_ROLE_DETACHED;
static volatile uint8_t    s_fault = 0;
static volatile bool       s_reset = false;
static volatile uint32_t   s_joiner_flash_until = 0;   // ms; 0 = not flashing

static inline void led_write(int gpio, int active_low, bool on)
{
    gpio_set_level((gpio_num_t)gpio, (on ^ (active_low != 0)) ? 1 : 0);
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void led_task(void *arg)
{
    for (;;) {
        const uint32_t t = now_ms();

        // ------------------- Commissioner LED -------------------
        bool comm_on;
        if (s_joiner_flash_until && (int32_t)(s_joiner_flash_until - t) > 0) {
            comm_on = ((t / 150) % 2) == 0;          // ~2 quick flashes over the window
        } else {
            s_joiner_flash_until = 0;
            switch (s_comm) {
                case LED_COMM_ACTIVE:      comm_on = true;                    break;
                case LED_COMM_PETITIONING: comm_on = ((t / 400) % 2) == 0;    break; // ~1.25 Hz
                default:                   comm_on = false;                   break; // DISABLED
            }
        }
        led_write(LED_COMM_GPIO, LED_COMM_ACTIVE_LOW, comm_on);

        // ------------------- System LED (priority) --------------
        bool sys_on;
        if (s_reset) {
            sys_on = ((t / 60) % 2) == 0;            // fast flutter (~8 Hz)
        } else if (s_fault) {
            // N short pulses (150 ms on / 150 ms off), then a 1 s gap, repeat.
            const uint32_t window = (uint32_t)s_fault * 300;
            const uint32_t p      = t % (window + 1000);
            sys_on = (p < window) && (((p / 150) % 2) == 0);
        } else {
            switch (s_role) {
                case LED_ROLE_LEADER:       sys_on = true;                 break;
                case LED_ROLE_CHILD_ROUTER: sys_on = ((t / 500) % 2) == 0; break; // 1 Hz
                default:                    sys_on = (t % 2000) < 60;      break; // DETACHED blip
            }
        }
        led_write(LED_SYS_GPIO, LED_SYS_ACTIVE_LOW, sys_on);

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_COMM_GPIO) | (1ULL << LED_SYS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    led_write(LED_COMM_GPIO, LED_COMM_ACTIVE_LOW, false);
    led_write(LED_SYS_GPIO,  LED_SYS_ACTIVE_LOW,  false);

    xTaskCreate(led_task, "led_task", 2048, NULL, 3, NULL);
}

void led_set_comm(led_comm_t state) { s_comm = state; }
void led_set_role(led_role_t role)  { s_role = role; }
void led_signal_joiner_added(void)  { s_joiner_flash_until = now_ms() + 600; }
void led_set_fault(uint8_t code)    { s_fault = code; }
void led_signal_reset(void)         { s_reset = true; }
