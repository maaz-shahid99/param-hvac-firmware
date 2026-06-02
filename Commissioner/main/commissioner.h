#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Start the Thread Commissioner
 * 
 * This function initiates the commissioner petition process.
 * The device must be a Leader or Router to become a Commissioner.
 */
void commissioner_start(void);

/**
 * @brief Stop the Thread Commissioner
 */
void commissioner_stop(void);

/**
 * @brief Add a joiner, self-healing if the commissioner session has dropped.
 *
 * If the commissioner is ACTIVE the joiner is added immediately. Otherwise the
 * joiner is queued, the commissioner is re-petitioned, and the joiner is added
 * automatically once the commissioner reaches the ACTIVE state.
 *
 * Prints the protocol response (`JOINER_ADDED <eui>` / `ERROR ADD_FAILED <n>` /
 * `COMMISSIONER_REPETITIONING`) so callers don't need to.
 *
 * @return true if the joiner was added immediately, false if queued/failed.
 */
bool commissioner_add_joiner(const char *eui64, const char *pskd, uint32_t timeout);

/**
 * @brief Check if Commissioner is active
 *
 * @return true if Commissioner is in ACTIVE state, false otherwise
 */
bool commissioner_is_active(void);

/**
 * @brief FreeRTOS task that waits for the thread stack to stabilize
 *        then starts the commissioner. Used after form_new_network().
 */
void delayed_commissioner_start_task(void *arg);
