#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PROJECT_DIR="${REPO_ROOT}/mcp_server"
BUILD_DIR="${REPO_ROOT}/build"
SERVER_BIN="${BUILD_DIR}/mcp_server"
CLIENT_BIN="${BUILD_DIR}/mcp_cpp_client"
SERVER_LOG_DIR="${BUILD_DIR}/logs"
CLIENT_LOG_DIR="${BUILD_DIR}/client_logs"
C_COMPILER="${CC:-$(command -v gcc-13 || command -v gcc-12 || command -v gcc)}"
CXX_COMPILER="${CXX:-$(command -v g++-13 || command -v g++-12 || command -v g++)}"

mkdir -p "${SERVER_LOG_DIR}" "${CLIENT_LOG_DIR}"

if [ ! -f "$SERVER_BIN" ]; then
    echo "错误: 未找到 mcp_server 可执行文件 ($SERVER_BIN)，请确保已提前编译好 Server。"
    exit 1
fi

if [ ! -f "$CLIENT_BIN" ]; then
    echo "错误: 未找到 mcp_cpp_client 可执行文件 ($CLIENT_BIN)，请先运行编译命令。"
    exit 1
fi

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "正在启动 MCP Server..."
"${SERVER_BIN}" -t -l "${SERVER_LOG_DIR}" -p "${BUILD_DIR}/plugins" \
  >"${BUILD_DIR}/mcp_server_stdout.log" 2>&1 &
SERVER_PID=$!

# 等待服务器就绪
sleep 3
if ! ps -p $SERVER_PID > /dev/null; then
    echo "错误: Server 启动失败。详情请查看 ${BUILD_DIR}/mcp_server_stdout.log"
    cat "${BUILD_DIR}/mcp_server_stdout.log"
    exit 1
fi
echo "Server 已启动 (PID: $SERVER_PID)"

"${CLIENT_BIN}" \
  --transport httpstream \
  --url "http://127.0.0.1:8080/mcp" \
  --log-dir "${CLIENT_LOG_DIR}" \
  --run-tests
