#ifndef AICAFLOW_FIRMWARE_H
#define AICAFLOW_FIRMWARE_H
#include <aicaflow/protocol.h>
afx_result_t afx_firmware_validate(const void *data, uint32_t size, afx_firmware_info_t *out);
#endif
