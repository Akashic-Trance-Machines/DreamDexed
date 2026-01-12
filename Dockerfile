FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies matching GitHub Actions runner
RUN apt-get update && apt-get install -y \
    wget \
    tar \
    make \
    git \
    zip \
    xz-utils \
    build-essential \
    python3 \
    bc \
    u-boot-tools \
    device-tree-compiler \
    texinfo \
    && rm -rf /var/lib/apt/lists/*

# Create directory for toolchains
WORKDIR /opt/arm

# Download toolchains based on architecture
RUN ARCH=$(uname -m) && \
    if [ "$ARCH" = "aarch64" ]; then \
        HOST_ARCH="aarch64"; \
    else \
        HOST_ARCH="x86_64"; \
    fi && \
    echo "Downloading toolchains for host architecture: $HOST_ARCH" && \
    \
    # 64-bit toolchain \
    wget -q "https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-aarch64-none-elf.tar.xz" && \
    tar xf "arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-aarch64-none-elf.tar.xz" && \
    rm "arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-aarch64-none-elf.tar.xz" && \
    \
    # 32-bit toolchain \
    wget -q "https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-arm-none-eabi.tar.xz" && \
    tar xf "arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-arm-none-eabi.tar.xz" && \
    rm "arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-arm-none-eabi.tar.xz" && \
    \
    # Create a nice PATH script \
    echo "export PATH=/opt/arm/arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-aarch64-none-elf/bin:/opt/arm/arm-gnu-toolchain-14.3.rel1-${HOST_ARCH}-arm-none-eabi/bin:\$PATH" > /etc/profile.d/arm-toolchains.sh

# Update PATH for the current image environment
# We include both possible paths so the ENV works regardless of arch (one set will just be invalid/ignored)
ENV PATH="/opt/arm/arm-gnu-toolchain-14.3.rel1-aarch64-aarch64-none-elf/bin:/opt/arm/arm-gnu-toolchain-14.3.rel1-aarch64-arm-none-eabi/bin:/opt/arm/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:/opt/arm/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin:${PATH}"

WORKDIR /dreamdexed
