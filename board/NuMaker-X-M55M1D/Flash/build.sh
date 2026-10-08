#!/usr/bin/env bash
# Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <NuMicroM55_DFP-root> <NuMicro_M55M1_BSP-root> <CMSIS-root>" >&2
  exit 2
fi

dfp_root=$1
bsp_root=$2
cmsis_root=$3
script_dir=$(cd "$(dirname "$0")" && pwd)
object_dir="$script_dir/Objects"
compiler_root=${AC6_TOOLCHAIN_ROOT:-/opt/Arm_Compiler_for_Embedded_6_24_0}
armclang="$compiler_root/bin/armclang"
armlink="$compiler_root/bin/armlink"

mkdir -p "$object_dir"

common_flags=(
  --target=arm-arm-none-eabi
  -mcpu=cortex-m55
  -mfloat-abi=hard
  -mfpu=fp-armv8-fullfp16-d16
  -O2
  -fropi
  -frwpi
  -ffunction-sections
  -fdata-sections
  -DM55M1_SERIES
  -D__MICROLIB
  -I"$dfp_root/Flash"
  -I"$dfp_root/Library/Device/Nuvoton/M55M1/Include"
  -I"$dfp_root/Library/StdDriver/inc"
  -I"$bsp_root/Board/NuMaker-X-M55M1D/HyperRAM"
  -I"$cmsis_root/CMSIS/Core/Include"
)

"$armclang" "${common_flags[@]}" -c "$script_dir/FlashPrg.c" -o "$object_dir/FlashPrg.o"
"$armclang" "${common_flags[@]}" -c "$script_dir/FlashDev.c" -o "$object_dir/FlashDev.o"
"$armclang" "${common_flags[@]}" -c "$dfp_root/Library/StdDriver/src/spim_hyper.c" -o "$object_dir/spim_hyper.o"
"$armclang" "${common_flags[@]}" -Dprintf=loader_printf -c "$bsp_root/Board/NuMaker-X-M55M1D/HyperRAM/hyperram_code.c" -o "$object_dir/hyperram_code.o"

"$armlink" \
  --scatter "$script_dir/Target.sct" \
  --entry 0x1 \
  --library_type=microlib \
  --diag_suppress L6305 \
  --map \
  --list "$object_dir/M55M1_HyperRAM.map" \
  --output "$script_dir/M55M1_HyperRAM.FLM" \
  "$object_dir/FlashPrg.o" \
  "$object_dir/FlashDev.o" \
  "$object_dir/spim_hyper.o" \
  "$object_dir/hyperram_code.o"

echo "built $script_dir/M55M1_HyperRAM.FLM"
