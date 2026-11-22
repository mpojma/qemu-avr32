/*
 * QEMU AVR32 UART
 *
 * Copyright (c) 2023, Florian Göhler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/avr32/at32uc3_uart.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"

/* AVR32 USART register offsets used by the firmware ASF driver and by the
 * pre-existing QEMU stub.  The stub used 0x1c for TX; keep it working for the
 * console while also accepting the ASF THR offset 0x00 for writes. */
#define AT32_USART_RHR 0x00  /* receive holding register (read) */
#define AT32_USART_THR 0x00  /* transmit holding register (write) */
#define AT32_USART_BRGR 0x04 /* baud rate generator register */
#define AT32_USART_CSR 0x14  /* channel status register (read) */
#define AT32_USART_STUB_THR 0x1c /* legacy stub TX offset */

/* CSR bits consumed by the firmware ASF driver (usart_is_rx_ready /
 * usart_is_tx_empty).  RXRDY is bit 0 in the AT32UC3 USART CSR; TXEMPTY is
 * bit 9 in ASF.  Bit 2 is also set as a conservative compatibility marker
 * because the original stub returned 0xFF here and some driver variants test
 * a different TX-idle bit. */
#define AT32_USART_CSR_RXRDY  (1U << 0)
#define AT32_USART_CSR_TXRDY  (1U << 1)
#define AT32_USART_CSR_TXEMPTY_COMPAT (1U << 2)
#define AT32_USART_CSR_TXEMPTY (1U << 9)

static void at32uc3_uart_update_irq(AT32UC3UARTState *s)
{
    bool assert_irq = s->rx_count > 0;
    if (assert_irq != s->irq_asserted) {
        s->irq_asserted = assert_irq;
        qemu_set_irq(s->irq, assert_irq ? 1 : 0);
    }
}

static uint8_t at32uc3_uart_fifo_pop(AT32UC3UARTState *s)
{
    uint8_t byte;
    if (s->rx_count == 0) {
        return 0;
    }
    byte = s->rx_fifo[s->rx_tail];
    s->rx_tail = (s->rx_tail + 1) & (AT32UC3_UART_RX_FIFO_SIZE - 1);
    s->rx_count--;
    at32uc3_uart_update_irq(s);
    return byte;
}

static void at32uc3_uart_tx_byte(AT32UC3UARTState *s, uint8_t byte)
{
    /* The legacy stub ignored 0x30 (ASCII '0').  Preserve that behaviour for
     * UART0 console output; GNSS traffic is binary and may contain 0x30, so
     * only apply the quirk in the non-chardev (legacy fprintf) path. */
    if (s->chardev_enabled) {
        qemu_chr_fe_write_all(&s->chr, &byte, 1);
    } else if (s->file_out) {
        if (byte != 0x30) {
            fputc(byte, s->file_out);
            fflush(s->file_out);
        }
    }
}

static uint64_t at32uc_uart_read(void *opaque, hwaddr addr, unsigned int size)
{
    AT32UC3UARTState *s = AT32UC3_UART(opaque);
    int offset = (int) addr;
    uint32_t returnValue = 0;

    switch (offset) {
    case AT32_USART_RHR:
        /* ASF reads RHR only when RXRDY is set, but return 0 when empty
         * rather than fabricating data. */
        returnValue = at32uc3_uart_fifo_pop(s);
        break;
    case AT32_USART_BRGR:
        returnValue = s->baudrate;
        break;
    case AT32_USART_CSR:
        returnValue = AT32_USART_CSR_TXRDY | AT32_USART_CSR_TXEMPTY |
                      AT32_USART_CSR_TXEMPTY_COMPAT;
        if (s->rx_count > 0) {
            returnValue |= AT32_USART_CSR_RXRDY;
        }
        break;
    default:
        break;
    }
    return returnValue;
}

static void at32uc_uart_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned int size)
{
    AT32UC3UARTState *s = AT32UC3_UART(opaque);
    int offset = (int) addr;

    switch (offset) {
    case AT32_USART_BRGR:
        s->baudrate = (uint32_t) val64;
        break;
    case AT32_USART_THR:
    case AT32_USART_STUB_THR:
        at32uc3_uart_tx_byte(s, (uint8_t) val64);
        break;
    case 0x24:
    case 0x28:
    default:
        break;
    }
}

static const MemoryRegionOps uart_ops = {
    .read = at32uc_uart_read,
    .write = at32uc_uart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4
    }
};

/* ---- chardev receive path ------------------------------------------------ */

static int uart_can_receive(void *opaque)
{
    AT32UC3UARTState *s = AT32UC3_UART(opaque);
    return AT32UC3_UART_RX_FIFO_SIZE - s->rx_count;
}

static void uart_receive(void *opaque, const uint8_t *buf, int size)
{
    AT32UC3UARTState *s = AT32UC3_UART(opaque);
    int i;

    for (i = 0; i < size; i++) {
        if (s->rx_count >= AT32UC3_UART_RX_FIFO_SIZE) {
            /* FIFO full: drop incoming bytes.  The guest is expected to
             * drain it promptly; dropping is safer than overwriting. */
            break;
        }
        s->rx_fifo[s->rx_head] = buf[i];
        s->rx_head = (s->rx_head + 1) & (AT32UC3_UART_RX_FIFO_SIZE - 1);
        s->rx_count++;
    }
    at32uc3_uart_update_irq(s);
}

static void uart_event(void *opaque, QEMUChrEvent event)
{
    /* Handle the standard close/disconnect events without dereferencing a
     * NULL backend; no state change is required for this byte-stream model. */
    (void) opaque;
    (void) event;
}

static void at32uc3_uart_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AT32UC3UARTState *s = AT32UC3_UART(dev);
    int i;

    if (s->chr.chr) {
        /* DEFINE_PROP_CHR's set_chr() already called qemu_chr_fe_init() when
         * the board set the "chardev" property; only install our handlers. */
        qemu_chr_fe_set_handlers(&s->chr, uart_can_receive, uart_receive,
                                 uart_event, NULL, s, NULL, true);
        s->chardev_enabled = true;
    } else {
        /* Legacy console UART0 behaviour: keep writing to uart_output.txt.
         * Only the instance explicitly configured for it opens the file, so
         * uart1 cannot truncate the console log. */
        s->chardev_enabled = false;
        if (s->legacy_output) {
            s->file_out = fopen("uart_output.txt", "w");
            if (!s->file_out) {
                error_setg(errp, TYPE_AT32UC3_UART ": failed to open uart_output.txt");
                return;
            }
        }
    }

    sysbus_init_irq(sbd, &s->irq);
    s->cs_lines = g_new0(qemu_irq, s->num_cs);
    for (i = 0; i < s->num_cs; ++i) {
        sysbus_init_irq(sbd, &s->cs_lines[i]);
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &uart_ops, s,
                          TYPE_AT32UC3_UART, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);

    s->irqline = -1;
}

static void at32uc3_uart_reset(DeviceState *dev)
{
    AT32UC3UARTState *s = AT32UC3_UART(dev);

    s->rx_head = 0;
    s->rx_tail = 0;
    s->rx_count = 0;
    s->baudrate = 0;
    s->irq_asserted = false;
    /* Reset may run before realize has initialized s->irq. */
    if (s->irq) {
        qemu_set_irq(s->irq, 0);
    }
}

static Property at32uc3_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", AT32UC3UARTState, chr),
    DEFINE_PROP_BOOL("legacy-output", AT32UC3UARTState, legacy_output, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void at32uc3_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = at32uc3_uart_realize;
    dc->reset = at32uc3_uart_reset;
    device_class_set_props(dc, at32uc3_uart_properties);
}

static const TypeInfo at32uc3_uart_info = {
    .name           = TYPE_AT32UC3_UART,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(AT32UC3UARTState),
    .class_init     = at32uc3_uart_class_init,
};

static void at32uc3_uart_register_types(void)
{
    type_register_static(&at32uc3_uart_info);
}

type_init(at32uc3_uart_register_types)
