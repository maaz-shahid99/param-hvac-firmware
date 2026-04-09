#pragma once

#include <stdbool.h>

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
