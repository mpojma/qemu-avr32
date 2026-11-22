/*
 * QEMU AVR32 Boot
 *
 * Copyright (c) 2022-2023 Florian Göhler, Johannes Willbold
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
#include "qemu/datadir.h"
#include "hw/loader.h"
#include "boot.h"
#include "qemu/error-report.h"
#include "elf.h"
#include <string.h>
#include "hw/core/tcg-cpu-ops.h"
#include "exec/exec-all.h"
#include "exec/address-spaces.h"
#include "target/avr32/helper_elf.h"
#include "target/avr32/cpu.h"
#include "hw/avr32/avr32_log.h"

void avr32_copy_text_section(int e_shnum, FILE* file, Elf32_Shdr** sh_table, char *sh_strtable, FILE* output){
    int text_section_idx = -1;
    for(int i= 0; i< e_shnum; i++){
        if(strcmp(&sh_strtable[sh_table[i]->sh_name], ".text") == 0){
            text_section_idx = i;
            break;
        }
    }
    if(text_section_idx >= 0){
        AVR32_LOG_DEV("[AVR32-BOOT] Text section is at index %i\n", text_section_idx);
    }
    else{
        error_report("[AVR32-BOOT] Unable to find .text section!\n");
        exit(1);
    }

    char *buffer = malloc(sh_table[text_section_idx]->sh_size);
    fseek(file, sh_table[text_section_idx]->sh_offset, SEEK_SET);
    int res = fread(buffer, 1, sh_table[text_section_idx]->sh_size, file);
    AVR32_LOG_DEV("[AVR32-BOOT] Loaded 0x%x bytes from .text section\n", res);
    fwrite(buffer, 1, res, output);
    free(buffer);
}

/* Load a section's bytes from the ELF file into the system address space at
 * the section's virtual address (sh_addr).  Used for .data (initialized
 * globals) and for zero-filling .bss. */
static void avr32_load_section_to_ram(int e_shnum, FILE *file,
                                      Elf32_Shdr **sh_table,
                                      char *sh_strtable, const char *name)
{
    int idx = -1;
    for (int i = 0; i < e_shnum; i++) {
        if (strcmp(&sh_strtable[sh_table[i]->sh_name], name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        AVR32_LOG_DEV("[AVR32-BOOT] Section %s not found, skipping\n", name);
        return;
    }

    uint32_t addr = sh_table[idx]->sh_addr;
    uint32_t size = sh_table[idx]->sh_size;
    if (size == 0) {
        AVR32_LOG_DEV("[AVR32-BOOT] Section %s has size 0, skipping\n", name);
        return;
    }

    char *buffer;
    if (sh_table[idx]->sh_type == SHT_NOBITS) {
        /* .bss: no bytes in the file, just zero-fill. */
        buffer = g_malloc0(size);
    } else {
        buffer = g_malloc(size);
        fseek(file, sh_table[idx]->sh_offset, SEEK_SET);
        size_t res = fread(buffer, 1, size, file);
        if (res != size) {
            error_report("[AVR32-BOOT] Short read for section %s "
                         "(%zu of 0x%x bytes)", name, res, size);
            g_free(buffer);
            return;
        }
    }

    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                        buffer, size);
    AVR32_LOG_DEV("[AVR32-BOOT] Loaded 0x%x bytes of %s at 0x%08x\n",
                  size, name, addr);
    g_free(buffer);
}

void avr32_copy_sections(int e_shnum, FILE* file, Elf32_Shdr** sh_table, char *sh_strtable, MemoryRegion *program_mr){
    /* .text goes into the flash MemoryRegion via the raw-image loader. */
    FILE *output = fopen("/tmp/qemu_avr32_tmp_text_sec", "wb");
    avr32_copy_text_section(e_shnum, file, sh_table, sh_strtable, output);
    fclose(output);

    int bytes_loaded = load_image_mr("/tmp/qemu_avr32_tmp_text_sec", program_mr);
    if (bytes_loaded < 0) {
        error_report("[AVR32-BOOT] Unable to load firmware image %s as raw binary",
                     "/tmp/qemu_avr32_tmp_text_sec");
        exit(1);
    }
    AVR32_LOG_DEV("[AVR32-BOOT] .text loaded into flash (%d bytes)\n", bytes_loaded);
    remove("/tmp/qemu_avr32_tmp_text_sec");

    /* .data (initialized globals) and .bss must live in SRAM at their ELF
     * virtual addresses (0x14 and 0x1930 for this firmware).  Loading them
     * concatenated after .text into flash left the global function pointers
     * zeroed, which made uffs_Mount() call through a NULL pointer. */
    avr32_load_section_to_ram(e_shnum, file, sh_table, sh_strtable, ".data");
    avr32_load_section_to_ram(e_shnum, file, sh_table, sh_strtable, ".bss");
}

bool avr32_load_elf_file(AVR32ACPU *cpu, const char *filename, MemoryRegion *program_mr){
    AVR32_LOG_DEV("[AVR32-BOOT] Loading firmware images as ELF file\n");

    Elf32_Ehdr header;
    FILE* file = fopen(filename, "rb");
    char *sh_strtable = 0;
    Elf32_Shdr** sh_table = 0;
    if(file) {
        // read the header
        int res = fread(&header, 1, sizeof(header), file);

        if(res <= 0){
            error_report("[AVR32-BOOT] Cannot read firmware image header\n");
            exit(1);
        }
        // check if it's really an elf file
        if (memcmp(header.e_ident, ELFMAG, SELFMAG) == 0) {
            avr32_convert_elf_header(&header);
            if(header.e_machine == EM_AVR32){
                sh_table = malloc(header.e_shnum * sizeof (Elf32_Shdr*));

                avr32_elf_read_section_headers(&header, file, sh_table);
                sh_strtable = (char *)malloc(sh_table[header.e_shstrndx]->sh_size * sizeof(char));

                avr32_elf_read_sh_string_table(&header, file, sh_table, sh_strtable);
                avr32_copy_sections(header.e_shnum, file, sh_table, sh_strtable, program_mr);

            }
            else{
                error_report("[AVR32-BOOT] Firmware file is not an AVR32 file!\n");
            }
        }
        else{
            error_report("[AVR32-BOOT] ELF file is not valid!\n");
            exit(1);
        }
    }

    fclose(file);
    free(sh_strtable);
    for(int i= 0; i< header.e_shnum; i++){
        free(sh_table[i]);
    }
    free(sh_table);
    return true;
}

bool avr32_load_firmware(AVR32ACPU *cpu, MachineState *ms,
                         MemoryRegion *program_mr, const char *firmware)
{
    AVR32_FIRMWARE_FILE = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);
    if (AVR32_FIRMWARE_FILE == NULL) {
        error_report("[AVR32-BOOT] Cannot find firmware image '%s'", firmware);
        return false;
    }

    if(avr32_is_elf_file(AVR32_FIRMWARE_FILE)){
        /* QEMU's generic ELF loader cannot handle this AVR32 ELF (see the
         * original TODO).  Use the custom loader, which now places .text in
         * the flash region and .data/.bss in SRAM at their ELF addresses. */
        if (!avr32_load_elf_file(cpu, AVR32_FIRMWARE_FILE, program_mr)) {
            return false;
        }
    }
    else{
        AVR32_LOG_DEV("[AVR32-BOOT]: Loading firmware images as raw binary\n");
        int bytes_loaded = load_image_mr(AVR32_FIRMWARE_FILE, program_mr);
        if (bytes_loaded < 0) {
            error_report("[AVR32-BOOT] Unable to load firmware image %s as raw binary",
                         firmware);
            return false;
        }
    }
    AVR32_LOG_DEV("[AVR32-BOOT] Loaded boot image successfully\n");

    return true;
}