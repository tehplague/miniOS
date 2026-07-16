#!/bin/sh
# Create a GPT partition table on an existing raw disk image.
# Usage: create-gpt-disk.sh <disk_img> <p1_start> <p1_size> <p2_start> <p2_size>
#
#   p1: Linux filesystem  (type 0FC63DAF-8483-4772-8E79-3D69D8477DE4)
#   p2: Microsoft basic data / FAT32  (type EBD0A0A2-B9E5-4433-87C0-68B6B72699C7)
#
# Data already written at p1_start and p2_start is not touched; sfdisk only
# writes the protective MBR, GPT header (LBA 0-1), partition entries (LBA 2-33),
# and the backup structures at the end of the image.

set -eu

disk_img="$1"
p1_start="$2"
p1_size="$3"
p2_start="$4"
p2_size="$5"

printf 'label: gpt\nstart=%s, size=%s, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="Linux filesystem"\nstart=%s, size=%s, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="FAT32 data"\n' \
    "$p1_start" "$p1_size" \
    "$p2_start" "$p2_size" | sfdisk --force -q "$disk_img"
