# Image to run tt-system-firmware CI tests.
# Includes python dependencies and base copy of repos

ARG ZEP_CONTAINER=v0.29.1
FROM ghcr.io/zephyrproject-rtos/zephyr-build:${ZEP_CONTAINER} AS base

USER root

# Patch APT urls to use https, since the container can't connect via HTTP
RUN sed -i -e 's/http/https/g' /etc/apt/sources.list && \
    apt-get update

# Install dependencies for openocd
RUN apt install -y libusb-1.0-0-dev

# Install Zephyr Cmake package for root
RUN cp /home/user/.cmake/ ~ -r

# Start a new stage- the openocd build artifacts are not needed
# in the final stage
FROM base AS openocd_build

ARG OPENOCD_REV=riscv-and-arc-rtt-20250213
ARG OPENOCD_URL=https://github.com/danieldegrasse/openocd.git

# Clone openocd and build it
RUN git clone ${OPENOCD_URL} -b ${OPENOCD_REV} openocd && \
    cd openocd && \
    ./bootstrap && \
    ./configure --prefix=/opt/openocd && \
    make -j$(nproc) &&  \
    make install

# Start a new stage, and copy openocd build artifacts
FROM base
LABEL org.opencontainers.image.source=https://github.com/tenstorrent/tt-system-firmware
COPY --from=openocd_build /opt/openocd/ /opt/toolchains/zephyr-sdk-$ZSDK_VERSION/sysroots/x86_64-pokysdk-linux/usr/

ENV ZEPHYR_BASE=/tt-zephyr/zephyr

ARG REPO_REV=main
ARG REPO_URL=https://github.com/tenstorrent/tt-system-firmware

# Clone the tt-system-firmware repo and install dependencies
RUN pip install west && \
    west init -m ${REPO_URL} --mr ${REPO_REV} /tt-zephyr && \
    cd /tt-zephyr/tt-system-firmware && \
    west update && \
    west packages pip --install && \
    pyocd pack install STM32G0B1CEUx
