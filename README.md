# CPP MCP-SERVER

一个基于 C++ 实现的模型上下文协议（Model Context Protocol）服务器，采用插件架构设计，支持多种传输协议，具备运行时插件热插拔能力。

## 项目架构

```
                          ┌─────────────────────┐
                          │      main.cpp       │
                          │    （启动入口）       │
                          └──────────┬──────────┘
                                     │
                  ┌──────────────────┼──────────────────┐
                  │                  │                   │
          ┌───────▼───────┐  ┌──────▼───────┐  ┌───────▼────────┐
          │    Server      │  │PluginsLoader │  │   Transport    │
          │  （请求调度）    │  │（插件管理）    │  │ （通信协议）    │
          └───────┬───────┘  └──────┬───────┘  └───────┬────────┘
                  │                 │                   │
        MCP 协议命令路由      动态库加载/监控      ┌─────┼─────┐
        回调覆盖机制          staging 副本机制    │     │     │
        通知队列发送          快照式并发访问     Stdio  SSE  HTTP
                  │                 │            Stream
                  │          ┌──────▼───────┐
                  └─────────►│   Plugins    │
                             │ （动态链接库） │
                             └──────────────┘
```

系统由四个核心模块组成：

- **Server**：实现 MCP 协议的请求解析、命令路由和响应构造，内置通知队列和异步写入线程
- **PluginsLoader**：负责插件的加载、卸载和热插拔，提供线程安全的快照访问接口
- **Transport**：抽象传输层，定义统一的读写接口，支持 Stdio / SSE / HTTP Stream 三种实现
- **Plugins**：以动态链接库形式存在的功能模块，通过 C 接口与宿主交互

## 代码结构

```
mcp_server/
│
├── CMakeLists.txt                  构建配置（C++20，版本 0.8.0）
├── version.h.in                    版本号模板，CMake 构建时生成 version.h
├── README.md                       本文档
├── 热插拔技术文档.md                热插拔功能详细设计文档
│
├── src/                            源码目录
│   ├── main.cpp                    入口：命令行解析、回调配置、启动流程
│   │
│   ├── server/                     MCP 服务器核心
│   │   ├── Server.h                Server 类声明、线程模型、命令注册表
│   │   └── Server.cpp              协议命令实现、请求分发、通知队列
│   │
│   ├── transport/                  传输层实现
│   │   ├── StdioTransport.h/cpp    标准输入输出传输（默认）
│   │   ├── SseTransport.h/cpp      Server-Sent Events 传输（HTTP 长连接）
│   │   └── HttpStreamTransport.hpp/cpp  HTTP 流式传输（会话制）
│   │
│   ├── loader/                     插件加载与热插拔
│   │   ├── PluginsLoader.h         PluginEntry 生命周期结构体、PluginsLoader 类
│   │   └── PluginsLoader.cpp       staging 加载、后台监控、三阶段扫描
│   │
│   ├── interface/                  接口定义
│   │   ├── ITransport.h            传输层抽象接口
│   │   └── PluginAPI.h             插件 C 接口（CreatePlugin/DestroyPlugin/HandleRequest）
│   │
│   └── utils/                      工具类
│       ├── MCPBuilder.h            MCP 响应和通知消息的 JSON 构造器
│       ├── SessionBuilder.h        会话 ID 生成器（时间戳 + 随机数）
│       └── TSingleton.h            线程安全单例模板
│
├── plugins/                        示例插件（各自编译为独立 .so/.dll）
│   ├── weather/                    天气查询工具（调用 open-meteo.com API）
│   ├── sleep/                      延迟执行工具（模拟耗时操作）
│   ├── code-review/                代码审查工具
│   ├── bacio-quote/                语录工具
│   └── notification/               通知测试工具（演示进度和日志通知）
│
├── include/                        第三方头文件
│   └── httplib.h                   cpp-httplib HTTP 库
│
├── libs_tier_01/                   第三方依赖库
│   ├── aixlog-1.5.0/               日志库
│   ├── alpaca-0.2.1/               序列化库
│   ├── json.hpp                    nlohmann/json JSON 库
│   ├── base64.hpp                  Base64 编解码
│   └── popl.hpp                    命令行参数解析器
│
└── test/                           测试
    ├── test-client-stdio.py        Stdio 传输测试客户端
    ├── test-client-sse.py          SSE 传输测试客户端
    ├── test-client-http.py         HTTP Stream 传输测试客户端
    ├── configuration-*.json        各传输协议的测试配置
    ├── requirements.txt            Python 测试依赖
    └── install_dependencies.sh     依赖安装脚本
```

## 核心模块详解

### Server（请求调度）

Server 维护一个 `method → handler` 的命令路由表，覆盖了 MCP 协议定义的所有命令：

| 类别 | 命令 | 说明 |
|------|------|------|
| 生命周期 | `initialize`、`ping` | 握手和心跳 |
| 工具 | `tools/list`、`tools/call` | 列出和调用工具插件 |
| 提示词 | `prompts/list`、`prompts/get` | 列出和获取提示词插件 |
| 资源 | `resources/list`、`resources/read` | 列出和读取资源插件 |
| 通知 | `notifications/tools/list_changed` 等 | 能力列表变更通知 |

`main.cpp` 通过 `OverrideCallback()` 将 `tools/list`、`tools/call` 等命令的默认实现替换为从 PluginsLoader 获取插件快照并遍历执行的逻辑。

Server 内部有一个独立的 **Writer 线程**，负责从通知队列中取出消息并通过 Transport 发送给客户端，避免通知发送阻塞请求处理。

### Transport（传输协议）

三种传输实现都继承自 `ITransport` 接口：

| 传输协议 | 通信方式 | 端口 | 适用场景 |
|----------|----------|------|----------|
| **Stdio** | stdin/stdout | 无 | Claude Desktop 等本地客户端 |
| **SSE** | HTTP POST(请求) + GET SSE(推送) | 8080 | 浏览器和 Web 客户端 |
| **HTTP Stream** | POST(请求) + GET SSE(通知) + 会话管理 | 8080 | REST API 客户端、多客户端场景 |

通过命令行参数选择：
```bash
./mcp_server                   # 默认 Stdio
./mcp_server -s                # SSE
./mcp_server -t                # HTTP Stream
```

### PluginsLoader（插件管理与热插拔）

插件加载器负责插件的完整生命周期管理，核心流程：

**启动加载**：扫描指定目录 → 复制到 staging → dlopen → CreatePlugin → Initialize → 设置通知系统

**运行时热插拔**：后台线程每 5 秒扫描目录变化，自动处理：
- 新增 `.so` 文件 → 加载新插件
- 文件 mtime/size 变化 → 先加载新版本，成功后替换旧版本
- 文件被删除 → 卸载插件

**并发安全**：请求线程通过 `GetPluginsSnapshot()` 获取插件列表的 `shared_ptr` 快照，在无锁状态下执行插件代码。旧插件通过引用计数延迟释放，直到所有使用它的请求完成。

详细设计参见 [热插拔技术文档.md](热插拔技术文档.md)。

### 插件接口（PluginAPI）

插件通过 C 接口与宿主交互，每个插件编译为独立的动态链接库，导出两个函数：

```c
PluginAPI* CreatePlugin();      // 创建插件实例
void DestroyPlugin(PluginAPI*); // 销毁插件实例
```

`PluginAPI` 结构体包含一组函数指针：

| 函数指针 | 说明 |
|----------|------|
| `GetName()` / `GetVersion()` | 插件名称和版本 |
| `GetType()` | 类型：TOOLS / PROMPTS / RESOURCES |
| `Initialize()` / `Shutdown()` | 生命周期管理 |
| `HandleRequest(json)` | 处理请求，返回 JSON 响应 |
| `GetToolCount()` / `GetTool(i)` | 工具注册（TOOLS 类型） |
| `GetPromptCount()` / `GetPrompt(i)` | 提示词注册（PROMPTS 类型） |
| `GetResourceCount()` / `GetResource(i)` | 资源注册（RESOURCES 类型） |

插件还可以通过 `NotificationSystem` 主动向客户端发送通知。

## 支持的平台

| 平台 | 编译器 |
|------|--------|
| Windows | MINGW64 |
| Ubuntu Linux | GCC |
| Mac OS | GCC |

## 如何编译

```bash
git clone <your-repo-url>
cd mcp_server
mkdir build && cd build
cmake ..
make
```

编译产物：
- `build/mcp_server` — 服务器可执行文件
- `build/plugins/` — 各插件的 `.so` / `.dll` 文件

## 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-n, --name` | 服务器名称 | `mcp-server` |
| `-p, --plugins` | 插件目录路径 | `./plugins` |
| `-l, --logs` | 日志目录路径（**目录必须存在**） | `./logs` |
| `-v, --verbose` | 启用详细日志 | `false` |
| `-s, --sse` | 使用 SSE 传输 | — |
| `-t, --httpstream` | 使用 HTTP Stream 传输 | — |

## Claude Desktop 配置

```json
{
  "mcpServers": {
    "mcp-server": {
      "command": "/path/to/mcp_server",
      "args": [
        "-n", "developer-server",
        "-l", "/path/to/logs",
        "-p", "/path/to/plugins"
      ],
      "env": {
        "CUSTOM_API_KEY": "your-api-key-here"
      }
    }
  }
}
```

## Claude Code 配置

```bash
claude mcp add --transport http cpp_mcp_server http://localhost:8080/mcp
```

## 线程模型

服务器运行时有以下线程：

```
主线程          请求处理主循环（阻塞在 transport->Read()）
Writer 线程     从通知队列取出消息并发送（condition_variable 等待）
Watcher 线程    每 5 秒扫描插件目录（热插拔启用时）
```

三者通过 `shared_mutex`（插件列表）和 `mutex + condition_variable`（通知队列）协调。


