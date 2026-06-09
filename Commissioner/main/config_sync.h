#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Mesh-wide application/Wi-Fi credential replication service.
 *
 * Distributes the gateway/app configuration (Wi-Fi SSID+pass, zone, Thread
 * network name) to EVERY node over the Thread mesh so any unit can take over
 * the gateway/uplink role. Transport is UDP multicast on the mesh-local
 * all-nodes group (ff03::1); every blob is HMAC-SHA256 signed and carries a
 * monotonic version for anti-rollback. Persisted in NVS on each node.
 *
 * Resilience model:
 *   - Provision one bridge -> its C6 calls config_sync_publish_local().
 *   - The blob is signed + multicast; every C6 stores it and relays it to its
 *     own C3 over UART ("CFG_SET ...").
 *   - Late/offline nodes pull the latest blob on boot (CFGREQ) and the Leader
 *     re-announces periodically, so the fleet is eventually consistent.
 *   - Each C6 also tells its C3 whether it is the active gateway via
 *     "GW_ROLE LEADER" / "GW_ROLE STANDBY", tied to Thread leadership.
 */
void config_sync_init(void);

/**
 * @brief Publish locally-provisioned credentials to the whole mesh.
 *
 * Assigns the next version (stored+1), signs, persists to NVS, relays to the
 * local C3, and multicasts to the mesh. Call this when this node receives
 * fresh credentials from its own C3 (i.e. it is the unit the installer
 * provisioned). Safe to call from a non-OpenThread task.
 *
 * @param pin Admin PIN to replicate fleet-wide, or "-" to leave it unchanged.
 */
void config_sync_publish_local(const char *ssid, const char *pass,
                               const char *zone, const char *net_name,
                               const char *pin);

/**
 * @brief Broadcast a signed fleet-OTA command over the mesh.
 *
 * Sends "OTA|<nonce>|<baseurl>|<hmac>" to the mesh-local all-nodes group. Every
 * node verifies it and relays "OTA_NOW <baseurl>" to its own C3, which then
 * self-updates from <baseurl>/firmware/. Also triggers the local C3 (so the
 * gateway updates too). @param baseurl e.g. "http://10.14.98.109:8001".
 */
void config_sync_broadcast_ota(const char *baseurl);

/**
 * @brief Broadcast a signed fleet factory-reset command over the mesh.
 *
 * Sends "RESET|<nonce>|<hmac>" to the mesh-local all-nodes group. Every node
 * verifies it and relays "RESET_NOW" to its own C3, which wipes NVS on both
 * chips and reboots. Also triggers the local C3 (so the gateway resets too).
 */
void config_sync_broadcast_reset(void);

/**
 * @brief Relay a BME environmental sample from our C3 toward the gateway/cloud.
 *
 * The C3 hands us "ENV <t>,<h>,<p>,<voc>" over UART; we tag it with our own
 * factory EUI (so it matches the device roster) and deliver it: the Leader
 * (active gateway) prints it straight to its own C3 for cloud forwarding; a
 * plain router relays it over the mesh to the gateway's UDP listener (port 1234).
 * Safe to call from a non-OpenThread task (acquires the lock itself).
 */
void config_sync_send_env(const char *payload);

/**
 * @brief Relay a firmware crash report from our C3 toward the gateway/cloud.
 * Same routing as config_sync_send_env; payload is "<reset>|<pc>|<bt>".
 */
void config_sync_send_crash(const char *payload);
