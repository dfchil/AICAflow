#pragma once
#include <stdint.h>
typedef void (*asic_evt_handler)(uint32_t code, void *data);
typedef struct { asic_evt_handler hdl; void *data; } asic_evt_handler_entry_t;
#define ASIC_EVT_SPU_IRQ 0x0101
#define ASIC_IRQ_DEFAULT 9
asic_evt_handler_entry_t asic_evt_set_handler(uint16_t code, asic_evt_handler handler, void *data);
void asic_evt_remove_handler(uint16_t code);
void asic_evt_enable(uint16_t code, uint8_t irqlevel);
void asic_evt_disable(uint16_t code, uint8_t irqlevel);
