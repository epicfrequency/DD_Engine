#!/usr/bin/env bash
set -euo pipefail

# Prefer command-line arg (htop visible): --rate 352800|384000
# Fallback to legacy env MPD_DYNAMIC_RATE, then default to 384000.
BASE_RATE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rate)
      BASE_RATE="${2:-}"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

if [[ -z "${BASE_RATE}" ]]; then
  BASE_RATE="${MPD_DYNAMIC_RATE:-384000}"
fi

# Validate numeric
if ! [[ "${BASE_RATE}" =~ ^[0-9]+$ ]]; then
  echo "[SDM Engine] ERROR: invalid BASE_RATE='${BASE_RATE}'" >&2
  exit 2
fi

# Only allow the two expected rates (optional but recommended)
if [[ "${BASE_RATE}" != "352800" && "${BASE_RATE}" != "384000" ]]; then
  echo "[SDM Engine] ERROR: unexpected BASE_RATE='${BASE_RATE}' (expected 352800 or 384000)" >&2
  exit 3
fi

# Derive DSD clock and buffer
CLOCK=$(( BASE_RATE * 2 ))
DYNAMIC_BUFFER=$(( BASE_RATE / 2 ))

# Print info (goes to MPD/systemd logs)
echo "[SDM Engine] New Stream Detected:"
echo " >> Base Rate:   ${BASE_RATE} Hz"
echo " >> Clock (2x):  ${CLOCK} Hz"
echo " >> Buffer Size: ${DYNAMIC_BUFFER} frames"

# Make htop show the rate clearly (argv[0])
NAME="sdm_${BASE_RATE}"

# Run pipeline:
# - SDM reads PCM float from stdin (MPD pipe)
# - outputs DSD_U32_BE to stdout
# - aplay consumes raw DSD stream
exec -a "${NAME}" /usr/local/bin/sdm5_mt 0.2 \
  | /usr/bin/aplay \
      -D hw:0,0 \
      -c 2 \
      -f DSD_U32_BE \
      -r "${CLOCK}" \
      --buffer-size="${DYNAMIC_BUFFER}" \
      -M \
      -t raw \
      -q
