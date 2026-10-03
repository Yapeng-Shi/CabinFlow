#!/usr/bin/env bash
set -euo pipefail
umask 077

[[ $# == 1 && ( $1 == start || $1 == stop ) ]] || { echo "用法：bash $0 start|stop" >&2; exit 2; }
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
build="$repo/runtime/build/voice-demo"
run="$build/launcher"
server="$build/voice_demo_server"
ui="$build/frontend/voice_demo_ui"
mkdir -p "$run"
exec 9> "$run/command.lock"
flock -n 9 || { echo '另一次启停正在执行' >&2; exit 1; }

alive() {
    local line
    [[ -d /proc/$1 ]] || return 1
    IFS= read -r line < "/proc/$1/stat" || return 0
    # 僵尸已经退出；读取失败则继续等，不提前声称清理完成。
    [[ ${line##*) } != Z\ * && ${line##*) } != X\ * ]]
}

stop_process() {
    local pid=$1 exe=$2 deadline attempt
    [[ -n $pid ]] || return 0
    [[ $pid =~ ^[1-9][0-9]*$ ]] && (( pid > 1 )) || return 1
    alive "$pid" || return 0
    # 刚启动的孩子可能仍在 nohup；最多等 1 秒，不把其他程序当成目标。
    for attempt in {1..10}; do
        [[ $(readlink "/proc/$pid/exe") == "$exe" ]] && break
        alive "$pid" || return 0
        sleep 0.1
    done
    # 只停止本项目二进制、当前用户的进程；不使用 pkill/killall。
    [[ $(readlink "/proc/$pid/exe") == "$exe" && $(stat -c %u "/proc/$pid") == "$EUID" ]] || return 1
    kill -TERM "$pid" || { alive "$pid" && return 1 || return 0; }
    deadline=$((SECONDS + 30))
    while alive "$pid"; do
        (( SECONDS < deadline )) || { echo "PID=$pid 尚未退出，未强杀，记录保留" >&2; return 1; }
        sleep 0.1
    done
}

if [[ $1 == stop ]]; then
    [[ -e $run/server.pid || -e $run/ui.pid ]] || { echo '没有本脚本的运行记录'; exit 0; }
    for role in server ui; do
        [[ -f $run/$role.pid ]] || continue
        pid=$(< "$run/$role.pid")
        exe=$server; [[ $role == server ]] || exe=$ui
        stop_process "$pid" "$exe" || { echo "$role 身份不符或停止未完成" >&2; exit 1; }
        rm -f "$run/$role.pid"
    done
    echo '已停止本脚本记录的 Demo，日志保留。'
    exit 0
fi

[[ ! -e $run/server.pid && ! -e $run/ui.pid ]] || { echo '已有运行记录，请先执行 stop' >&2; exit 1; }
[[ -x $server && -x $ui ]] || { echo '请先构建 Demo' >&2; exit 1; }
[[ -n ${WAYLAND_DISPLAY:-} && -n ${XDG_RUNTIME_DIR:-} ]] || { echo '请在 WSLg 终端运行' >&2; exit 1; }
logs=$(mktemp -d "$run/run-XXXXXX")
server_pid=''; ui_pid=''; interrupted=0
cleanup() {
    trap - EXIT INT TERM
    for role in server ui; do
        pid=$server_pid; exe=$server
        [[ $role == server ]] || { pid=$ui_pid; exe=$ui; }
        if stop_process "$pid" "$exe"; then rm -f "$run/$role.pid"; else
            echo "清理未完成：$role PID=$pid；请检查 $logs" >&2
            break
        fi
    done
}
trap cleanup EXIT
trap 'interrupted=1' INT TERM

echo "启动后端，日志：$logs"
nohup "$server" 127.0.0.1 39001 \
    "$repo/agent/voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16" \
    "$repo/runtime/build/models/qwen3-0.6b/Qwen3-0.6B-Q8_0.gguf" \
    "$repo/runtime/build/models/vits-melo-tts-zh_en" </dev/null > "$logs/server.log" 2>&1 9>&- &
server_pid=$!
printf '%s\n' "$server_pid" > "$run/server.pid"
deadline=$((SECONDS + 120))
until grep -q '^voice_demo_server listening=127\.0\.0\.1:39001 ' "$logs/server.log"; do
    (( interrupted == 0 )) || exit 130
    alive "$server_pid" && (( SECONDS < deadline )) || { echo '后端未就绪，请检查日志' >&2; exit 1; }
    sleep 0.1
done
(( interrupted == 0 )) || exit 130
export QT_QPA_PLATFORM=wayland
nohup "$ui" 127.0.0.1 39001 </dev/null > "$logs/ui.log" 2>&1 9>&- &
ui_pid=$!
printf '%s\n' "$ui_pid" > "$run/ui.pid"
sleep 2
(( interrupted == 0 )) || exit 130
alive "$server_pid" && alive "$ui_pid" || { echo 'Demo 提前退出，请检查日志' >&2; exit 1; }
trap - EXIT INT TERM
echo "Demo 已启动。停止：bash $repo/agent/scripts/voice_demo.sh stop"
