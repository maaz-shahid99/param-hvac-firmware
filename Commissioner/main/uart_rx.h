#pragma once

#include "esp_openthread.h"

void uart_rx_init(void);

// Initialize the OpenThread stack
void thread_init(void);

// Form a new Thread network with the given name (Used by UART command)
void form_new_network(const char *network_name); 
