#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_PATH="${1:-${SCRIPT_DIR}/src/frontend/decky/assets/test-sample.flac}"

command -v ffmpeg > /dev/null 2>&1 || {
    echo "ERROR: ffmpeg is required to generate the listening sample." >&2
    exit 1
}

mkdir -p "$(dirname "${OUTPUT_PATH}")"

LEFT_EXPRESSION='0.12*sin(PI*t/8)^2*between(t\,0\,8)*(sin(2*PI*110*t)+0.35*sin(2*PI*220*t))+0.09*sin(PI*(t-8)/8)^2*between(t\,8\,16)*(sin(2*PI*440*t)+0.45*sin(2*PI*880*t)+0.2*sin(2*PI*1320*t))+0.055*sin(PI*(t-16)/8)^2*between(t\,16\,24)*(sin(2*PI*2800*t)+0.4*sin(2*PI*4200*t))+0.018*sin(PI*(t-24)/8)^2*between(t\,24\,32)*(sin(2*PI*180*t)+sin(2*PI*900*t)+0.6*sin(2*PI*3600*t))'
RIGHT_EXPRESSION='0.12*sin(PI*t/8)^2*between(t\,0\,8)*(sin(2*PI*140*t)+0.35*sin(2*PI*280*t))+0.09*sin(PI*(t-8)/8)^2*between(t\,8\,16)*(sin(2*PI*520*t)+0.45*sin(2*PI*1040*t)+0.2*sin(2*PI*1560*t))+0.055*sin(PI*(t-16)/8)^2*between(t\,16\,24)*(sin(2*PI*3200*t)+0.4*sin(2*PI*4800*t))+0.018*sin(PI*(t-24)/8)^2*between(t\,24\,32)*(sin(2*PI*220*t)+sin(2*PI*1100*t)+0.6*sin(2*PI*4000*t))'

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi \
    -i "aevalsrc=${LEFT_EXPRESSION}|${RIGHT_EXPRESSION}:s=44100:d=32" \
    -c:a flac \
    "${OUTPUT_PATH}"

printf 'Created %s\n' "${OUTPUT_PATH}"
