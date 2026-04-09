#include "udp_listener.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/instance.h"
#include "openthread/udp.h"
#include "openthread/ip6.h"
#include <string.h>

static const char *TAG = "UDP_RX";

#define UDP_LISTEN_PORT 1234

static otUdpSocket sUdpSocket;
static bool sSocketOpen = false;

static void udp_receive_callback(void *aContext, otMessage *aMessage,
                                 const otMessageInfo *aMessageInfo)
{
    char buf[256];
    uint16_t len = otMessageGetLength(aMessage) - otMessageGetOffset(aMessage);

    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, len);
    buf[len] = '\0';

    // Print the sender's IPv6 address
    char addrStr[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&aMessageInfo->mPeerAddr, addrStr, sizeof(addrStr));

    ESP_LOGW(TAG, "=== SENSOR DATA RECEIVED ===");
    ESP_LOGW(TAG, "From: [%s]:%d", addrStr, aMessageInfo->mPeerPort);
    ESP_LOGW(TAG, "Data (%d bytes): %s", len, buf);
    ESP_LOGW(TAG, "============================");

    // Also print to stdout so it shows on the serial monitor plainly
    printf("[UDP_RX] From [%s]:%d -> %s\n", addrStr, aMessageInfo->mPeerPort, buf);
    fflush(stdout);
}

void udp_listener_start(void)
{
    if (sSocketOpen) {
        ESP_LOGI(TAG, "UDP listener already running on port %d", UDP_LISTEN_PORT);
        return;
    }

    otInstance *instance = esp_openthread_get_instance();

    memset(&sUdpSocket, 0, sizeof(sUdpSocket));

    otError err = otUdpOpen(instance, &sUdpSocket, udp_receive_callback, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to open UDP socket: %d", err);
        return;
    }

    otSockAddr bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.mPort = UDP_LISTEN_PORT;
    // Bind to all addresses (in6addr_any)

    err = otUdpBind(instance, &sUdpSocket, &bindAddr, OT_NETIF_UNSPECIFIED);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to bind UDP socket to port %d: %d", UDP_LISTEN_PORT, err);
        otUdpClose(instance, &sUdpSocket);
        return;
    }

    sSocketOpen = true;
    ESP_LOGW(TAG, "*** UDP Listener ACTIVE on port %d ***", UDP_LISTEN_PORT);
}
