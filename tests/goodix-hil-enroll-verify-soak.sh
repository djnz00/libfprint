#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
workspace_root=$(CDPATH= cd -- "$source_root/.." && pwd)

build_dir=${LIBFPRINT_BUILD_DIR:-"$source_root/build"}
iterations=${GOODIX_HIL_SOAK_ITERATIONS:-3}
finger_choice=${GOODIX_HIL_FINGER_CHOICE:-6}
artifact_root=${GOODIX_HIL_SOAK_ARTIFACT:-"$workspace_root/arc/$(date -u +%Y%m%dT%H%M%SZ)-27c6-521d-enroll-verify-soak"}

if [[ ! -x "$build_dir/examples/enroll" || ! -x "$build_dir/examples/verify" ]]; then
  echo "Expected built examples under $build_dir/examples" >&2
  echo "Set LIBFPRINT_BUILD_DIR=/path/to/libfprint/build if needed." >&2
  exit 2
fi

if ! command -v script >/dev/null 2>&1; then
  echo "script(1) is required so hardware prompts remain visible while logging." >&2
  exit 2
fi

mkdir -p "$artifact_root"

common_env=(
  "LD_LIBRARY_PATH=$build_dir/libfprint"
  "FP_DRIVERS_ALLOWLIST=goodixtls52xd"
  "G_MESSAGES_DEBUG=${G_MESSAGES_DEBUG:-all}"
  "LIBUSB_DEBUG=${LIBUSB_DEBUG:-0}"
)

run_logged() {
  local name=$1
  local input=$2
  local command=$3
  local iter_dir=$4
  local log="$iter_dir/$name.log"
  local raw_dir="$iter_dir/raw-dump"

  mkdir -p "$raw_dir"
  echo
  echo ">>> $name: prompts are live; follow the sensor instructions."

  # Use script(1) to keep prompts visible and capture the full terminal log.
  script -qefc \
    "cd '$iter_dir' && printf '%b' '$input' | sudo -n env GOODIX52XD_DUMP_DIR='$raw_dir' ${common_env[*]} '$command'" \
    "$log"
}

for ((i = 1; i <= iterations; i++)); do
  iter_dir="$artifact_root/iter-$i"
  mkdir -p "$iter_dir"

  echo
  echo "=== Goodix enroll/verify soak iteration $i/$iterations ==="
  echo "Finger choice $finger_choice is preselected. Answering no to update prompts."

  run_logged "enroll" "${finger_choice}\nn\n" "$build_dir/examples/enroll" "$iter_dir"
  run_logged "verify" "${finger_choice}\nn\n" "$build_dir/examples/verify" "$iter_dir"

  if ! grep -q "Enroll stage 5 of 5 passed" "$iter_dir/enroll.log"; then
    echo "Enrollment did not complete all five stages; see $iter_dir/enroll.log" >&2
    exit 1
  fi

  if ! grep -q "MATCH!" "$iter_dir/verify.log"; then
    echo "Verification did not report MATCH; see $iter_dir/verify.log" >&2
    exit 1
  fi

  if grep -q "Deactivating image device while it is not idle" "$iter_dir/enroll.log" "$iter_dir/verify.log"; then
    echo "Image device deactivated while non-idle; see $iter_dir" >&2
    exit 1
  fi

done

cat > "$artifact_root/summary.txt" <<EOF
Goodix 27c6:521d enroll/verify HIL soak
Iterations: $iterations
Build dir: $build_dir
Result: PASS
EOF

echo
echo "Goodix HIL soak passed: $artifact_root"
