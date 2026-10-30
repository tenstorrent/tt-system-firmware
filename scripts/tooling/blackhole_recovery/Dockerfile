# Image to run blackhole recovery tooling.
# Includes python dependencies, a built recovery bundle, and
# necessary recovery scripts

ARG ZEP_CONTAINER=v0.29.1

# Source tree the image is built from. By default this clones REPO_URL at
# REPO_REV (how releases are built). To build from a local checkout instead,
# e.g. to test a PR end to end, override this stage with a named context:
#   docker build --build-context repo-src=<checkout> ...
FROM ghcr.io/zephyrproject-rtos/zephyr-build:${ZEP_CONTAINER} AS repo-clone
USER root
ARG REPO_REV=main
ARG REPO_URL=https://github.com/tenstorrent/tt-system-firmware
RUN git clone --branch ${REPO_REV} ${REPO_URL} /src

FROM scratch AS repo-src
COPY --from=repo-clone /src /

FROM ghcr.io/zephyrproject-rtos/zephyr-build:${ZEP_CONTAINER} AS build-image

USER root

# Patch APT urls to use https, since the container can't connect via HTTP
RUN sed -i -e 's/http/https/g' /etc/apt/sources.list && \
    apt-get update

# Install Zephyr Cmake package for root
RUN cp /home/user/.cmake/ ~ -r

ENV ZEPHYR_BASE=/tt-zephyr/zephyr

# Parse signing key secret, if one was provided. Without it, images are
# signed with MCUboot's default development key (e.g. CI test builds).
RUN --mount=type=secret,id=signing_key \
    if [ -s /run/secrets/signing_key ]; then \
        base64 -d /run/secrets/signing_key > /signing_key.pem; \
    fi

# Set up a west workspace around the tt-system-firmware tree and install dependencies
COPY --from=repo-src / /tt-zephyr/tt-system-firmware
RUN pip install west && \
    west init -l /tt-zephyr/tt-system-firmware && \
    cd /tt-zephyr/tt-system-firmware && \
    west update && \
    west packages pip --install

# Build the blackhole recovery image
RUN cd /tt-zephyr/tt-system-firmware && \
    python3 ./scripts/tooling/blackhole_recovery/prepare-recovery-bundle.py /recovery.tar.gz \
        $([ -s /signing_key.pem ] && echo --signing-key /signing_key.pem)

# Start a new stage, and copy the built recovery bundle
FROM ubuntu:24.04
LABEL org.opencontainers.image.source=https://github.com/tenstorrent/tt-system-firmware
COPY --from=build-image /recovery.tar.gz /recovery.tar.gz

ARG PYTHON_VENV_PATH=/opt/python/venv

# Install python and create a venv
RUN apt-get update && \
    apt-get install -y python3 python3-pip python3-venv && \
	mkdir -p ${PYTHON_VENV_PATH} && \
	python3 -m venv ${PYTHON_VENV_PATH}

# Make python venv available in PATH
ENV PATH=${PYTHON_VENV_PATH}/bin:$PATH

# Install usb dependencies
RUN apt-get install -y libusb-1.0-0

# Install python dependencies in the venv
RUN pip install pyocd pyluwen pyyaml pykwalify protobuf imgtool \
        jsonschema intelhex pyusb && \
    pyocd pack install STM32G0B1CEUx

# Recovery scripts come from the same tree the bundle was built from
COPY --from=repo-src / /tt-system-firmware
