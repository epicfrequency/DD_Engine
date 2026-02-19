#!/usr/bin/env bash
set -euo pipefail

# --- 参数解析与校验 (保持原有逻辑) ---
BASE_RATE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rate) BASE_RATE="${2:-}"; shift 2 ;;
    *) shift ;;
  esac
done

if [[ -z "${BASE_RATE}" ]]; then
  BASE_RATE="${MPD_DYNAMIC_RATE:-384000}"
fi

if ! [[ "${BASE_RATE}" =~ ^[0-9]+$ ]]; then
  echo "[SDM Engine] ERROR: invalid BASE_RATE" >&2
  exit 2
fi

# --- 核心计算 ---
CLOCK=$(( BASE_RATE * 2 ))
# 建议：如果是为了极低延迟，buffer 可以尝试减小，但当前设置比较稳
DYNAMIC_BUFFER=$(( BASE_RATE * 1 ))

# --- 性能定义 ---
CORE=2          # 固定到 Core 2
RT_PRIO=95      # 实时优先级 (最高 99，建议 95 留一点余量给内核中断)

echo "[SDM Engine] Launching on Core ${CORE} with RT Priority ${RT_PRIO}"
echo " >> Base: ${BASE_RATE} | Clock: ${CLOCK} | Buffer: ${DYNAMIC_BUFFER}"

# 进程显示名称
NAME="sdm_${BASE_RATE}"

# --- 核心改进：全链路优先级加速 ---
# 1. 使用 taskset -c 锁定物理核心
# 2. 使用 chrt -f 设置 SCHED_FIFO 实时调度
# 3. 注意：aplay 也必须给高优先级，否则管道会因为输出端堵塞而产生 jitter

exec -a "${NAME}" \
  taskset -c ${CORE} \
  chrt -f ${RT_PRIO} \
  /usr/local/bin/sdm5_mt 0.2 \
  | chrt -f ${RT_PRIO} \
    /usr/bin/aplay \
      -D hw:0,0 \
      -c 2 \
      -f DSD_U32_BE \
      -r "${CLOCK}" \
      --buffer-size="${DYNAMIC_BUFFER}" \
      -M \
      -t raw \
      -q

