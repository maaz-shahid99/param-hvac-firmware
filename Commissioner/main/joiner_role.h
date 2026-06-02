#pragma once

/**
 * @brief Device-side Thread Joiner for a router-eligible (FTD) device.
 *
 * This is the counterpart to the commissioner: instead of FORMING a network,
 * the device scans for an existing network and asks to be commissioned into it
 * using its pre-shared device key (PSKd). Once the handshake succeeds the
 * network credentials are persisted to NVS and the device reboots to attach
 * as a Full Thread Device (which the stack then promotes to Router).
 *
 * Call this at boot ONLY when no operational dataset exists yet. A device that
 * is explicitly told to FORM a network (FORM_NET) must NOT run as a joiner.
 */
void router_joiner_start(void);

/**
 * @brief Stop the joiner role (and its retry task).
 *
 * Used when this device is designated as the network former (FORM_NET) after
 * it had already started scanning as a joiner.
 *
 * NOTE: The caller must hold the OpenThread lock (or be running in the
 * OpenThread task context before the mainloop is launched).
 */
void router_joiner_stop(void);
