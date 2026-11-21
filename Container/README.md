# NanoMind A3200 emulation with QEMU (avr32-nanomind)

This repository contains a QEMU 8.0.50 fork that emulates the **NanoMind A3200**
flight hardware (AT32UC3C0512C, 32 MHz, 512 KB flash, 64 KB SRAM, 32 MB SDRAM,
SPI NOR flash, FM33256B FRAM, I2C sensors, GNSS on USART1) and runs the flight
firmware `nanomind_flash.elf` unmodified.

The git repository with the source and Docker build is `qemu-avr32/`, branch
`avr32-nanomind`, at https://github.com/mpojma/qemu-avr32.

---

## 1. Build the emulator image

From this directory (`E:\qemu-avr32`):

```cmd
podman build -f Container/Dockerfile -t qemu-avr32-local .
```

The Dockerfile clones only the needed submodules, so the first build takes a
few minutes; subsequent builds use the layer cache. Tag the result as
`qemu-avr32-local` (all commands below use that name).

To inspect the container instead of running QEMU directly:

```cmd
podman run --rm -it --entrypoint bash qemu-avr32-local
```

---

## 2. Build the firmware (separate container)

The firmware is NOT built by this repository. It is built from
`E:\workspace_avs\CiC_afsa` with the SDK builder image:

```cmd
podman run --rm -t -v E:/workspace_avs/CiC_afsa:/workspace yocto_thud_cic_builder bash -c "./waf build"
```

The output used by QEMU is `E:\workspace_avs\CiC_afsa\build\nanomind_flash.elf`.
All examples below mount that `build` directory at `/workspace` inside QEMU.

---

## 3. Run the firmware

### 3.1 Basic boot with logging

```cmd
podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace ^
  qemu-avr32-local ^
  -M nanomind-a3200 -nographic ^
  -bios /workspace/nanomind_flash.elf ^
  -d avr32_cpu,avr32_dev -D /workspace/qemu.log
```

`-nographic` routes the guest serial console to the terminal. Log categories:

- `avr32_disas` — instruction disassembly
- `avr32_cpu` — CPU events
- `avr32_dev` — device activity (FRAM, SDRAMC, WDT, HMC5843, boot)

`-d help` lists them. At runtime, from the QEMU monitor (`Ctrl+a c`, then type):

```
log avr32_dev     ! enable a category
log none          ! disable all
```

Note: `log -avr32_dev` does **not** work in this 8.0.50 fork; use `log none`.

### 3.2 FRAM persistence

QEMU loads the backing file into the FRAM model when the device is realized.
Use a raw 32768-byte file and `if=none` so QEMU does not auto-attach it:

```cmd
python -c "open(r'E:\workspace_avs\CiC_afsa\build\fram.bin','wb').write(bytes(32768))"

podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace ^
  qemu-avr32-local ^
  -M nanomind-a3200,fram-drive-id=fram0 -nographic ^
  -bios /workspace/nanomind_flash.elf ^
  -drive if=none,file=/workspace/fram.bin,format=raw,id=fram0 ^
  -d avr32_dev -D /workspace/qemu_fram.log
```

The board property `fram-drive-id=fram0` resolves the backend by id. If it is
omitted, the board falls back to `drive_get(IF_MTD, 0, 1)`. A zero-filled
`fram.bin` makes the firmware apply its built-in defaults cleanly (the param
store sees an invalid header and falls back). With `avr32_dev` logging you will
see a line like:

```
[fm33256b] loaded 32768 bytes from drive; first8=0000000000000000 sum=0x00000000
```

Do **not** use `if=mtd` for the FRAM: the board already takes the first `if=mtd`
for the NOR flash and a second one conflicts ("Drive 'nor0' is already in use").

### 3.3 GNSS on USART1 (Python TCP simulator)

USART1 is modelled with a `qemu_chr_fe` socket. Start the Python GNSS server on
the host (see `docs/T-011-python-gnss-uart-connection.md`), then run QEMU with a
socket chardev. From inside a Podman container, reach the host via
`host.containers.internal`:

```cmd
podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace ^
  qemu-avr32-local ^
  -M nanomind-a3200,gnss-chardev=gnss -nographic ^
  -bios /workspace/nanomind_flash.elf ^
  -chardev socket,id=gnss,host=host.containers.internal,port=5000 ^
  -d avr32_dev -D /workspace/qemu_gnss.log
```

Note: the correct board property is `gnss-chardev=gnss` (singular), not the
plural form.

---

## 4. Known status / caveats

- The firmware boots past the scheduler and reaches the software watchdog,
  HMC5843 init and FRAM. See `docs/T-010-gap-analysis.md` for the full
  datasheet-vs-emulator gap analysis and the root-cause write-up of the
  MCALL return-address bug (fixed in commit `7429add`).
- With the MCALL fix the firmware enters a fast loop reading the MCU WDT CTRL
  register; whether this is intended watchdog polling or a remaining model gap
  is still under investigation.
- Some peripherals (LM71 temperature sensors, ADC, PWM, CAN, PPS) are stubs or
  missing; they are documented as future tasks in `docs/T-010-gap-analysis.md`.
- The firmware files in `E:\workspace_avs\CiC_afsa` are never modified by this
  project.

## 5. Repository layout

- `Container/Dockerfile` — canonical container build (this directory).
- `Container/Dockerfile.local`, `Dockerfile.logging` — deprecated incremental
  variants, kept for reference only.
- `qemu-avr32/` — the QEMU source git repository (branch `avr32-nanomind`).
  - `contrib/docker/Dockerfile` — in-repo copy of the container build.
  - `docs/T-010-gap-analysis.md` — hardware/emulator gap analysis and root cause.
  - `docs/T-011-python-gnss-uart-connection.md` — GNSS UART design.
