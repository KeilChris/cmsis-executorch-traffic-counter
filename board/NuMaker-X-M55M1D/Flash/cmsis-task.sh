#!/usr/bin/env bash
# Copyright 2026 Arm Limited and/or its affiliates. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <pyocd-action> <generated-cbuild-run> [pyocd-options...]" >&2
  exit 2
fi

action=$1
generated_runner=$2
shift 2

script_dir=$(cd "$(dirname "$0")" && pwd)
workspace_dir=$(cd "$script_dir/../../.." && pwd)
runner_name=$(basename "$generated_runner" .cbuild-run.yml)
prepared_runner="$workspace_dir/out/${runner_name}.hyperram.cbuild-run.yml"
selected_runner=$(python3 "$script_dir/prepare_cbuild_run.py" "$generated_runner" "$prepared_runner")

exec pyocd "$action" "$@" --cbuild-run "$selected_runner"
