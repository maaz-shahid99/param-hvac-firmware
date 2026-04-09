#pragma once

/**
 * Open a UDP socket on port 1234 bound to the mesh-local address.
 * Must be called while the OT lock is held OR from the OT main thread.
 */
void udp_listener_start(void);
