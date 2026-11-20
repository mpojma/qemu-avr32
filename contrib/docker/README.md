# Docker image for qemu-avr32 (nanomind branch)

This directory contains the canonical container build for the AVR32 QEMU fork
used to emulate the NanoMind A3200 flight hardware.

## Files

- `Dockerfile` — self-contained build from this repository's source tree.
- `README.md` — this file.

A `.dockerignore` at the repository root keeps `.git/` and local `build/`
artifacts out of the build context, so the image is reproducible from a clean
checkout.

## Build

From the **repository root** (`E:\qemu-avr32\qemu-avr32` on this machine):

```cmd
podman build -f contrib/docker/Dockerfile -t qemu-avr32-local .
```

If you prefer Docker:

```cmd
docker build -f contrib/docker/Dockerfile -t qemu-avr32-local .
```

## Run

Mount the固件 build directory so the guest can see `nanomind_flash.elf`:

```cmd
podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace ^
  qemu-avr32-local ^
  -M nanomind-a3200 -nographic ^
  -bios /workspace/nanomind_flash.elf ^
  -d avr32_cpu,avr32_dev -D /workspace/qemu.log
```

### With FRAM persistence

QEMU loads the backing file at device realize time. Use `if=none` and resolve
the backend with the board property `fram-drive-id`:

```cmd
podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace ^
  qemu-avr32-local ^
  -M nanomind-a3200,fram-drive-id=fram0 -nographic ^
  -bios /workspace/nanomind_flash.elf ^
  -drive if=none,file=/workspace/fram.bin,format=raw,id=fram0
```

`fram.bin` must be 32768 bytes (FM33256B FRAM). A zero-filled file makes the
firmware apply its built-in defaults cleanly.

### With the GNSS UART

USART1 is wired to a `qemu_chr_fe` socket. Connect it to the Python GNSS
simulator with the board property `gnss-chardev`:

```cmd
-M nanomind-a3200,gnss-chardev=gnss ^
-chardev socket,id=gnss,host=host.containers.internal,port=5000
```

## Logging

Log categories are `avr32_disas`, `avr32_cpu` and `avr32_dev`.

- Enable at startup: `-d avr32_cpu,avr32_dev -D /workspace/qemu.log`
- Toggle at runtime from the monitor: `log avr32_dev`, `log none`
- List categories: `-d help`

`log -avr32_dev` does **not** work in this QEMU 8.0.50 fork; use `log none`.

## Notes

The legacy files `Container/Dockerfile.local` and `Container/Dockerfile.logging`
in the parent directory are deprecated. This Dockerfile supersedes them: all
fixes (leveled logging, FRAM persistence, PM stub, USART1/GNSS, MCALL fix) are
committed in the source tree and built directly.
