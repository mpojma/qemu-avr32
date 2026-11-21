/*
 * QEMU AVR32 Nanomind A3200
 *
 * Copyright (c) 2023, Florian Göhler, Johannes Willbold
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
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "at32uc3.h"
#include "boot.h"
#include "qom/object.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "sysemu/block-backend.h"
#include "chardev/char.h"
#include "at32uc3_twim.h"
#include "hw/avr32/avr32_log.h"

/* Chardev id for the GNSS USART (USART1).  Set with -gnss-chardev <id> after
 * defining the chardev with -chardev socket,id=<id>,... */
static char *gnss_chardev_id;

/* BlockBackend id for the persistent FRAM (SPI1).  Set with
 * -M nanomind-a3200,fram-drive-id=<id> after defining an mtd drive. */
static char *fram_drive_id;

static char *nanomind_get_fram_drive_id(Object *obj, Error **errp)
{
    (void) obj;
    (void) errp;
    return g_strdup(fram_drive_id ? fram_drive_id : "");
}

static void nanomind_set_fram_drive_id(Object *obj, const char *value,
                                       Error **errp)
{
    (void) obj;
    (void) errp;
    g_free(fram_drive_id);
    fram_drive_id = g_strdup(value);
}

static char *nanomind_get_gnss_chardev(Object *obj, Error **errp)
{
    (void) obj;
    (void) errp;
    return g_strdup(gnss_chardev_id ? gnss_chardev_id : "");
}

static void nanomind_set_gnss_chardev(Object *obj, const char *value,
                                      Error **errp)
{
    (void) obj;
    (void) errp;
    g_free(gnss_chardev_id);
    gnss_chardev_id = g_strdup(value);
}

struct NanomindA3200MachineState {
    /*< private >*/
    MachineState parent_obj;
    /*< public >*/
    AT32UC3CSocState soc;

    MemoryRegion ebi_sdram;
    DeviceState* spi_flash[2];
    DeviceState* fram[2];
};
typedef struct NanomindA3200MachineState NanomindA3200MachineState;

struct NanomindA3200MachineClass {
    /*< private >*/
    MachineClass parent_class;
    /*< public >*/

    int sdram_size;
    int spi_flash_size;
    int config_fram_size;

};
typedef struct NanomindA3200MachineClass NanomindA3200MachineClass;

#define TYPE_NANOMIND_A3200_BASE_MACHINE MACHINE_TYPE_NAME("nanomind-a3200-base")

#define TYPE_NANOMIND_A3200_MACHINE MACHINE_TYPE_NAME("nanomind-a3200")
DECLARE_OBJ_CHECKERS(NanomindA3200MachineState, NanomindA3200MachineClass,
        NANOMIND_A3200_MACHINE, TYPE_NANOMIND_A3200_MACHINE)


static void nanomind_3200_init(MachineState *machine)
{
    SSIBus *ssi;

    NanomindA3200MachineClass* nmmc = NANOMIND_A3200_MACHINE_GET_CLASS(machine);
    nmmc->sdram_size = 32 * MiB;
    nmmc->spi_flash_size = 128 * MiB;
    nmmc->config_fram_size = 32 * KiB;

    NanomindA3200MachineState* nmms = NANOMIND_A3200_MACHINE(machine);

    AVR32_LOG_DEV("Setting up board...\n");

    /* Resolve the optional persistent FRAM backend before realizing devices.
     * Prefer the explicit id; otherwise fall back to the second MTD drive by
     * order (matching the historical drive-get convention on this board). */
    BlockBackend *fram_blk = NULL;
    if (fram_drive_id && *fram_drive_id) {
        fram_blk = blk_by_name(fram_drive_id);
        if (!fram_blk) {
            error_report("fram-drive-id: no block backend with id '%s'",
                         fram_drive_id);
            exit(1);
        }
    } else {
        DriveInfo *dinfo = drive_get(IF_MTD, 0, 1);
        fram_blk = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;
    }

    object_initialize_child(OBJECT(machine), "soc", &nmms->soc, TYPE_AT32UC3C0512C_SOC);

    /* Bind the optional GNSS chardev to USART1 before the SoC is realized;
     * UART0 keeps the legacy console behaviour (no chardev bound). */
    if (gnss_chardev_id && *gnss_chardev_id) {
        Chardev *gnss_chr = qemu_chr_find(gnss_chardev_id);
        if (!gnss_chr) {
            error_report("gnss-chardev: no chardev with id '%s'", gnss_chardev_id);
            exit(1);
        }
        qdev_prop_set_chr(DEVICE(&nmms->soc.uart1), "chardev", gnss_chr);
        AVR32_LOG_DEV("GNSS USART1 bound to chardev '%s'\n", gnss_chardev_id);
    }

    sysbus_realize(SYS_BUS_DEVICE(&nmms->soc), &error_abort);

    const int spi_num_buses = 2;
    for(int i = 0; i < spi_num_buses; ++i) {
        qemu_irq cs_line;
        ssi = (SSIBus *) qdev_get_child_bus(DEVICE(&nmms->soc.spi[i]), "spi");

        // Create the SPI-connected flash storage
        nmms->spi_flash[i] = qdev_new("s25fl512s");

        // This sets the actual underlying memory interface for the flash chip
        DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
        if (dinfo) {
            qdev_prop_set_drive_err(nmms->spi_flash[i], "drive",
                                    blk_by_legacy_dinfo(dinfo),
                                    &error_fatal);
        }
        qdev_realize_and_unref(nmms->spi_flash[i], BUS(ssi), &error_fatal);

        cs_line = qdev_get_gpio_in_named(nmms->spi_flash[i], SSI_GPIO_CS, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(&nmms->soc.spi[i]), 0 + 1, cs_line);

        // Create the SPI-connected FRAM
        nmms->fram[i] = qdev_new("fm33256b");

        /* A BlockBackend cannot be shared by two devices.  The board has two
         * FRAM instances; only the one used by firmware (SPI1, i == 1) gets
         * the persistent backend.  SPI0 keeps a private zero-filled buffer. */
        if (i == 1 && fram_blk) {
            qdev_prop_set_drive_err(DEVICE(nmms->fram[i]), "drive",
                                    fram_blk, &error_fatal);
        }
        qdev_realize_and_unref(nmms->fram[i], BUS(ssi), &error_fatal);

        cs_line = qdev_get_gpio_in_named(nmms->fram[i], SSI_GPIO_CS, 0);
        // FRAM fm33256b is at irq 3 + 1
        sysbus_connect_irq(SYS_BUS_DEVICE(&nmms->soc.spi[i]), 3 + 1, cs_line);
    }

    DeviceState* dev;

    // NanoMind Internal I2C
    // 3 Axis Gyroscope
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "mpu3300", 0x68));
    (void) dev;
    // 3 Axis Compass
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "hmc5843", 0x1e));
    (void) dev;
    // Magnetic Sensor
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "rm3100", 0x20));
    (void) dev;
    // Backup RM3100
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "rm3100", 0x48));
    (void) dev;

    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "htpa16x4.eeprom", 0x50));
    (void) dev;
    // Sun Sensor?
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "htpa16x4", 0x60));
    (void) dev;

    // There seem to be multiple GSSB istage things
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "gssb.istage", 0x18));
    (void) dev;
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "gssb.istage", 0x1a));
    (void) dev;
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "gssb.istage", 0x1c));
    (void) dev;
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "gssb.istage", 0x1f));
    (void) dev;
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[2].bus, "gssb.istage", 0x22));
    (void) dev;

    //BUS I2C with CSP implementation
    /* NanoPower P31U (EPS) I2C address is 0x20 per the firmware's
     * p31u_client/eps_board.h (EPS_BOARD_I2C_ADDR), not 0x2. */
    dev = DEVICE(i2c_slave_create_simple(nmms->soc.twim[0].bus, "nanopower.p31u", 0x20));
    (void) dev;

    AVR32_LOG_DEV("Board setup complete\n");
    AVR32_LOG_DEV("Loading firmware '%s'...\n", machine->firmware);

    if (machine->firmware) {
        if (!avr32_load_firmware(&nmms->soc.cpu.cpu, machine,
                                 &nmms->soc.onChipFlash, machine->firmware)) {
            exit(1);
        }
    }
}

static void nanomind_3200_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "GomSpace NanoMind A3200";
    mc->alias = "nanomind-a3200";
    mc->init = nanomind_3200_init;
    mc->default_cpus = 1;
    mc->min_cpus = mc->default_cpus;
    mc->max_cpus = mc->default_cpus;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(oc, "gnss-chardev",
                                  nanomind_get_gnss_chardev,
                                  nanomind_set_gnss_chardev);
    object_class_property_set_description(oc, "gnss-chardev",
        "Chardev id to attach to the GNSS USART (USART1)");

    object_class_property_add_str(oc, "fram-drive-id",
                                  nanomind_get_fram_drive_id,
                                  nanomind_set_fram_drive_id);
    object_class_property_set_description(oc, "fram-drive-id",
        "BlockBackend id of the mtd drive to use as persistent FRAM (SPI1)");
}

static const TypeInfo nanomind_a3200_machine_types[] = {
        {
                .name           = TYPE_NANOMIND_A3200_MACHINE,
                .parent         = TYPE_MACHINE,
                .instance_size  = sizeof(NanomindA3200MachineState),
                .class_size     = sizeof(NanomindA3200MachineClass),
                .class_init     = nanomind_3200_class_init,
        }
};

DEFINE_TYPES(nanomind_a3200_machine_types)
