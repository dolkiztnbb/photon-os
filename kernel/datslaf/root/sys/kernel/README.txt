PhotonOS kernel storage - /sys/kernel

This directory contains bootable kernel images.

Files:
  kernel.bin - current default kernel (copied from init.bin at build time)
  kernel-<version>.bin - versioned kernels (e.g., kernel-1.0.0.bin)
  kernel-<date>.bin - dated builds
  boot.cfg / default / active - text files containing filename of kernel to boot by default
  versions.txt - human-readable list of available versions

Bootloader behavior (stage2):
  1. Tries to read /sys/kernel from filesystem (DATSLAF)
  2. If boot.cfg/default/active exists, reads filename from it and tries to load that kernel
  3. Otherwise tries kernel.bin
  4. If multiple kernels found, shows menu to select version (0-9)
  5. If no FS kernel found, falls back to LBA 60 (old init.bin location)

Installer / version management:
  - To install new kernel: copy new kernel binary to /sys/kernel/ (e.g., kernel-1.1.0.bin)
  - Update boot.cfg to point to new file, or let bootloader menu select it
  - Old kernels remain for rollback
  - Future installer can manage this directory automatically

This enables:
  - Multiple kernel versions on same disk
  - Safe updates with rollback
  - Installer support
  - Kernel stored inside filesystem, not at fixed disk location
