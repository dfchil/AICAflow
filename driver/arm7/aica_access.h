/* How the executor reaches AICA RAM. On the ARM7 an AICA address is a
 * pointer; a test harness or a host build defines AICA_RAM first to map it.
 * (The host backend extends this header with register access.) */
#ifndef AICAFLOW_AICA_ACCESS_H
#define AICAFLOW_AICA_ACCESS_H
#include <aicaflow/protocol.h>

/* An AICA RAM address as something to cast to a pointer. */
#ifndef AICA_RAM
#define AICA_RAM(address) ((uintptr_t)(address))
#endif
#endif
