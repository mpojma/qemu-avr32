#ifndef HW_AVR32_AVR32_LOG_H
#define HW_AVR32_AVR32_LOG_H

#include "qemu/log.h"

#define AVR32_LOG_DISAS(...) qemu_log_mask(LOG_AVR32_DISAS, __VA_ARGS__)
#define AVR32_LOG_CPU(...)   qemu_log_mask(LOG_AVR32_CPU, __VA_ARGS__)
#define AVR32_LOG_DEV(...)   qemu_log_mask(LOG_AVR32_DEV, __VA_ARGS__)

#endif
