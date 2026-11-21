# QEMU avr32-nanomind — build from the canonical remote (recommended)
# ====================================================================
# Build context: E:\qemu-avr32  (this directory's parent). The context is
# intentionally almost empty; .dockerignore keeps the local checkout out.
#
#   podman build -f Container/Dockerfile -t qemu-avr32-local .
#
# The image clones the avr32-nanomind branch and initialises ONLY the five
# submodules QEMU actually needs to compile (dtc, meson, keycodemapdb and the
# two berkeley float test suites). A full --recursive clone would also pull
# edk2/openssl/etc. (hundreds of MB) which are irrelevant for avr32-softmmu.
#
# All fixes (leveled logging, FRAM persistence, PM stub, USART1/GNSS, MCALL
# return-address fix) are already committed on that branch.

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

# Clone only the branch; then pull the 5 submodules needed for the build.
RUN git clone --depth 1 -b avr32-nanomind \
        https://github.com/mpojma/qemu-avr32.git /src \
    && cd /src \
    && git submodule update --init --depth 1 \
        dtc meson ui/keycodemapdb \
        tests/fp/berkeley-testfloat-3 tests/fp/berkeley-softfloat-3

WORKDIR /src/build
RUN ../configure --target-list=avr32-softmmu --disable-docs --disable-werror \
    && make -j"$(nproc)" \
    && make install

WORKDIR /workspace
ENTRYPOINT ["qemu-system-avr32"]
CMD ["--version"]
