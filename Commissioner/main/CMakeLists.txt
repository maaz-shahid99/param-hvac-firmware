idf_component_register(
    SRCS
        "main.c"
        "thread_init.c"
        "commissioner.c"
        "uart_rx.c"
        "security.c"
        "joiner_manager.c"
        "udp_listener.c"
    INCLUDE_DIRS "."
    REQUIRES
        openthread
        esp_netif
        esp_event
        nvs_flash
        driver
        mbedtls
)
