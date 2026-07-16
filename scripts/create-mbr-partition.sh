#!/bin/sh

set -eu

disk_img="$1"
start_sector="$2"

printf '%s,+,83,\n' "$start_sector" | sfdisk --force -q "$disk_img"
