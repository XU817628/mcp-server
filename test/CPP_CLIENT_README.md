# C++ MCP Client

本目录新增了一个可直接编译运行的 C++ 版 MCP 客户端测试程序 `mcp_cpp_client`，用于对当前项目的 `mcp_server` 做全流程协议验证。

## 实现范围

- 支持 `HTTP Stream` 传输，适配当前项目 `GET /mcp` SSE 长连接 + `POST /mcp` 请求/通知 + `DELETE /mcp` 会话关闭机制
- 支持项目中保留的 `SSE` 兼容模式，适配 `GET /sse` + `POST /messages`
- 封装当前服务端已注册的全部标准 MCP 接口：
  - `initialize`
  - `ping`
  - `tools/list`
  - `tools/call`
  - `resources/list`
  - `resources/read`
  - `resources/subscribe`
  - `resources/unsubscribe`
  - `prompts/list`
  - `prompts/get`
  - `logging/setLevel`
  - `completion/complete`
  - `roots/list`
  - `notifications/initialized`
  - `notifications/cancelled`
  - `notifications/progress`
  - `notifications/roots/list_changed`
  - `notifications/resources/list_changed`
  - `notifications/resources/updated`
  - `notifications/prompts/list_changed`
  - `notifications/tools/list_changed`
  - `notifications/message`

## 日志能力

- 所有客户端发送 JSON 请求体均按原文输出
- 所有服务端返回 JSON 响应体均按原文输出
- 所有通过 SSE 接收的服务端通知原文均完整输出
- 日志同时写入终端和本地文件
- 日志文件按日期滚动，命名格式为 `mcp-client-YYYY-MM-DD.log`

## 构建

在主流 Linux 环境下执行：

```bash
cd /mnt/hgfs/Work_C/00AA-Start/Git_pull_mcp
C_COMPILER="$(command -v gcc-13 || command -v gcc-12 || command -v gcc)"
CXX_COMPILER="$(command -v g++-13 || command -v g++-12 || command -v g++)"
cmake -S mcp_server -B build \
  -DCMAKE_C_COMPILER="${C_COMPILER}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}"
cmake --build build -j"$(nproc)"
```

生成产物：

- `build/mcp_server`
- `build/mcp_cpp_client`

## 一键全流程测试

推荐直接运行：

```bash
cd /mnt/hgfs/Work_C/00AA-Start/Git_pull_mcp
bash mcp_server/test/run_cpp_mcp_http_tests.sh
```

该脚本会自动完成以下步骤：

1. 配置并编译项目
2. 启动 `mcp_server -t`
3. 建立 HTTP Stream 会话
4. 运行初始化、工具、资源、提示词、通知、订阅相关测试
5. 输出终端日志与本地日志文件
6. 自动关闭服务端进程

## 手动运行

先启动服务端：

```bash
cd /mnt/hgfs/Work_C/00AA-Start/Git_pull_mcp
C_COMPILER="$(command -v gcc-13 || command -v gcc-12 || command -v gcc)"
CXX_COMPILER="$(command -v g++-13 || command -v g++-12 || command -v g++)"
cmake -S mcp_server -B build \
  -DCMAKE_C_COMPILER="${C_COMPILER}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}"
cmake --build build -j"$(nproc)"
mkdir -p build/logs build/client_logs
./build/mcp_server -t -l ./build/logs -p ./build/plugins
```

再运行客户端测试：

```bash
./build/mcp_cpp_client \
  --transport httpstream \
  --url http://127.0.0.1:8080/mcp \
  --log-dir ./build/client_logs \
  --run-tests
```

## SSE 兼容模式

如果需要验证旧版 SSE 传输，可启动服务端：

```bash
./build/mcp_server -s -l ./build/logs -p ./build/plugins
```

然后运行：

```bash
./build/mcp_cpp_client \
  --transport sse \
  --url http://127.0.0.1:8080/sse \
  --log-dir ./build/client_logs \
  --run-tests
```

## 测试说明

`mcp_cpp_client --run-tests` 会覆盖以下场景：

- 初始化握手与会话建立
- `ping`
- 工具列表查询与工具调用
- 资源列表查询与资源读取
- Prompt 列表查询与 Prompt 获取
- 客户端通知上报
- 服务端 SSE 通知接收
- 当前服务端未实现接口的合规错误验证

其中未实现接口会校验服务端返回的 JSON-RPC 错误码 `-32601`，用于确认客户端封装和协议兼容性均正确。
