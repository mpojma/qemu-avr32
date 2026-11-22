# QEMU avr32-nanomind — self-contained container build
# ======================================================
#
# Build from the repository root (the build context is this git checkout):
#
#   podman build -f contrib/docker/Dockerfile -t qemu-avr32-local .
#
# See README.md in this directory for build, run, FRAM and GNSS instructions.
#
# This image clones the avr32-nanomind branch from the canonical remote and
# initialises ONLY the five submodules QEMU needs to compile. A full recursive
# clone would also pull edk2/openssl/etc. (hundreds of MB), which are irrelevant
# for avr32-softmmu. All fixes are committed on that branch. The .dockerignore
# at the context root keeps local git/build artifacts out of the context.

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

RUN git clone --depth 1 -b avr32-nanomind \
        https://github.com/mpojma/qemu-avr32.git /src \
    && cd /src \
    && git submodule update --init --depth 1 \
        dtc meson ui/keycodemapdb \
        tests/fp/berkeley-testfloat-3 tests/fp/berkeley-softfloat-3

# ALTERNATIVE for development with unpushed local commits: replace the RUN
# above with:   COPY . /src
# and build with the repository root as context. The local checkout must have
# its submodules initialised and no stale build/ directory.

WORKDIR /src/build
RUN ../configure --target-list=avr32-softmmu --disable-docs --disable-werror \
    && make -j"$(nproc)" \
    && make install

WORKDIR /workspace
ENTRYPOINT ["qemu-system-avr32"]
CMD ["--version"]
