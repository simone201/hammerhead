#!/bin/bash

ROOTFS_PATH="/home/simone/neak-nexus5/ramdisk"

echo "Building N.E.A.K. Nexus 5 for B2G..."

# Cleanup
./clean.sh

# Making .config
make neak_b2g_defconfig

# Compiling
./build.sh $ROOTFS_PATH
