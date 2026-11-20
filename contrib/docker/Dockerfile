# QEMU avr32-nanomind — reproducible container build
# =====================================================
#
# Build from the repository root (the build context is this git checkout):
#
#   podman build -f contrib/docker/Dockerfile -t qemu-avr32-local .
#
# Run the NanoMind A3200 firmware (no FRAM backing file):
#
#   podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace \
#       qemu-avr32-local \
#       -M nanomind-a3200 -nographic \
#       -bios /workspace/nanomind_flash.elf \
#       -d avr32_cpu,avr32_dev -D /workspace/qemu.log
#
# Run with FRAM persistence (block backend, id resolved by the board property):
#
#   podman run --rm -it -v E:/workspace_avs/CiC_afsa/build:/workspace \
#       qemu-avr32-local \
#       -M nanomind-a3200,fram-drive-id=fram0 -nographic \
#       -bios /workspace/nanomind_flash.elf \
#       -drive if=none,file=/workspace/fram.bin,format=raw,id=fram0
#
# Run with the emulated GNSS UART (USART1) connected to a Python TCP server:
#
#   -M nanomind-a3200,gnss-chardev=gnss \
#   -chardev socket,id=gnss,host=host.containers.internal,port=5000
#
# Useful:
#   podman run --rm -it --entrypoint bash qemu-avr32-local
#   qemu-system-avr32 -d help          # list available log categories
#   (qemu) log avr32_dev / log none    # toggle categories at runtime
#
# This image clones the avr32-nanomind branch from the canonical remote so the
# build is reproducible and uses the exact submodule versions referenced by
# the repository. For local, unpushed changes, see the alternative COPY line
# below. The .dockerignore at the context root keeps local artifacts out.

FROM ubuntu:22.04

LABEL org.opencontainers.image.title="qemu-avr32-nanomind" \
      org.opencontainers.image.description="QEMU 8.0.50 with the avr32-nanomind target" \
      org.opencontainers.image.source="https://github.com/mpojma/qemu-avr32"

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Europe/Madrid

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        flex \
        bison \
        gdb-multiarch \
        git \
        libglib2.0-dev \
        libpixman-1-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        tzdata \
        vim \
    && ln -snf /usr/share/zoneinfo/${TZ} /etc/localtime \
    && echo ${TZ} > /etc/timezone \
    && rm -rf /var/lib/apt/lists/*

# Clone the exact source tree (including the pinned submodules). This avoids
# stale local submodule checkouts breaking the build from a clean context.
RUN git clone --depth 1 --recursive --shallow-submodules \
        -b avr32-nanomind \
        https://github.com/mpojma/qemu-avr32.git /src

# ALTERNATIVE for development with unpushed local commits: comment the RUN
# above and uncomment the COPY below, then build with this directory as context.
# The local checkout must have its submodules initialised (git submodule update
# --init --recursive) and must NOT have a stale build/ directory.
# COPY . /src

WORKDIR /src/build
RUN ../configure --target-list=avr32-softmmu --disable-docs --disable-werror \
    && make -j"$(nproc)" \
    && make install

WORKDIR /workspace
ENTRYPOINT ["qemu-system-avr32"]
CMD ["--version"]
