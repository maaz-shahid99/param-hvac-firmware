#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Status-LED driver for the Commissioner (C6). Drives TWO of the router's four
// discrete LEDs (the other two — Power and Gateway/Uplink — are the 3V3 rail
// and the C3 Bridge respectively):
//
//   * COMMISSIONER LED  — external LED on GPIO21 (XIAO C6 silk "D3", active-HIGH)
//       off            = commissioner DISABLED
//       slow blink     = PETITIONING / re-petitioning
//       solid          = ACTIVE (ready for joiners)
//       2 quick flashes (one-shot) = a joiner was just added
//
//   * SYSTEM LED        — external LED on a free GPIO (active-HIGH by default)
//       render priority: reset flutter > fault N-pulse > role > heartbeat
//       brief blip / 2s = DETACHED (alive, no network yet)
//       1 Hz blink      = CHILD/ROUTER (attached, standby)
//       solid           = LEADER (this unit is the gateway brain)
//       N short pulses  = fault code N (repeating, after a 1s gap)
//       fast flutter    = factory reset in progress (latched until reboot)
//
// All setters are non-blocking and safe to call from any task / event-handler
// context (they only store a volatile word; a dedicated task renders).
// ---------------------------------------------------------------------------

typedef enum {
    LED_COMM_DISABLED = 0,   // off
    LED_COMM_PETITIONING,    // slow blink
    LED_COMM_ACTIVE,         // solid
} led_comm_t;

typedef enum {
    LED_ROLE_DETACHED = 0,   // brief blip every ~2s
    LED_ROLE_CHILD_ROUTER,   // 1 Hz heartbeat
    LED_ROLE_LEADER,         // solid
} led_role_t;

// Configure the GPIOs and start the render task. Call once from app_main.
void led_init(void);

void led_set_comm(led_comm_t state);   // Commissioner LED base state
void led_set_role(led_role_t role);    // System LED role base state
void led_signal_joiner_added(void);    // one-shot 2-flash on the Commissioner LED
void led_set_fault(uint8_t code);      // 0 = clear; 1..9 = repeating N-pulse on System LED
void led_signal_reset(void);           // latch the fast flutter (call just before reboot)
