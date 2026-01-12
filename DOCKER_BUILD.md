# Docker Build Environment for DreamDexed

This project provides a Docker-based build environment to ensure a consistent toolchain and simplify the build process.

## Prerequisites

- [Docker](https://www.docker.com/products/docker-desktop/) installed and running.
- A terminal (Bash, Zsh, or PowerShell).

## 1. Build the Docker Image

The `Dockerfile` contains all necessary dependencies and ARM toolchains. To build the image, run the following command from the project root:

```bash
docker build -t dreamdexed-builder .
```

This will create an image named `dreamdexed-builder` based on Ubuntu 22.04 with the GNU ARM Toolchain 14.3.

## 2. Run the Build

To build the project using the Docker container, you need to mount your source code directory into the container and run the `local-ci.sh` script.

### Build Command

Replace `[RPI_VERSION]` with your target Raspberry Pi version (e.g., 1, 2, 3, or 4).

```bash
docker run --rm -v "$(pwd):/dreamdexed" -e RPI=[RPI_VERSION] dreamdexed-builder bash local-ci.sh
```

**Example for Raspberry Pi 3:**
```bash
docker run --rm -v "$(pwd):/dreamdexed" -e RPI=3 dreamdexed-builder bash local-ci.sh
```

### What this does:
- `--rm`: Automatically removes the container after it exits.
- `-v "$(pwd):/dreamdexed"`: Mounts your current directory to `/dreamdexed` inside the container.
- `-e RPI=3`: Sets the environment variable for the target Raspberry Pi hardware.
- `bash local-ci.sh`: Executes the local CI script which handles submodules, toolchains, and the main build.

## 3. Build Output

After a successful build, the compiled files are located in the `./sdcard/` directory:

- `kernel*.img`: The compiled kernel for your Raspberry Pi.
- `minidexed.ini`: Configuration file.
- `performance/`: Performance data and voice banks.
- Bootloader files (`bootcode.bin`, `start.elf`, etc.).

## 4. Deployment

You can use the provided deployment script to copy these files to your SD card (replace `/path/to/sdcard` with the actual mount point):

```bash
DEST=/Volumes/UBUNTU ./deploy-sdcard.sh
```

## Troubleshooting

- **Architecture Mismatch**: The Dockerfile automatically detects `x86_64` or `aarch64` (Apple Silicon) host architectures and downloads the appropriate toolchain.
- **Permission Issues**: If you encounter permission issues on Linux, you may need to run Docker with `sudo` or add your user to the `docker` group.
- **Submodule Errors**: `local-ci.sh` automatically runs `./submod.sh`. Ensure you have a working internet connection for the first build.
