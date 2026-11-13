/* The firmware's executor (driver/arm7/driver.c), compiled natively for the
 * host and running on the software AICA. */
#ifndef AICAFLOW_HOST_FIRMWARE_H
#define AICAFLOW_HOST_FIRMWARE_H

#include "../emu/aica.h"

/* Boots the executor on chip, as the ARM7 does when its reset is released:
 * the firmware image uploaded at address 0 gives the layout it reports. */
void afx_host_firmware_boot(aica_t *chip);
/* One pass of the firmware's main loop: queued commands, then due events. */
void afx_host_firmware_pass(aica_t *chip);
/* The timer FIQ: advances the firmware's clock and rearms Timer A. */
void afx_host_firmware_timer(aica_t *chip);

#endif
