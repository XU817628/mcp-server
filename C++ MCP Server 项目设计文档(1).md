# C++ MCP Server 项目设计文档

> 面试导向 · 全知识点覆盖 · 架构与实现深度解析

---

## 一、项目概述

本项目是一个基于 **C++20** 实现的 **MCP（Model Context Protocol，模型上下文协议）服务器**，采用**插件架构**设计，支持 **Stdio / SSE / HTTP Stream** 三种传输协议，具备**运行时插件热插拔**能力。

MCP 是由 Anthropic 提出的开放协议，用于 LLM（大语言模型）与外部工具/资源之间的标准化通信。本服务器作为 MCP 协议的服务端实现，可以让 AI 客户端（如 Claude Desktop、Claude Code）通过统一协议调用后端工具。

---

## 二、项目架构

### 2.1 整体架构图

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
                  └─────────?│   Plugins    │
                             │ （动态链接库） │
                             └──────────────┘
```

### 2.2 分层架构

| 层次 | 模块 | 职责 |
|------|------|------|
| **入口层** | `main.cpp` | 命令行解析、组件组装、回调配置、生命周期管理 |
| **协议层** | `Server` | MCP 协议请求解析、命令路由、响应构造、通知队列 |
| **传输层** | `ITransport / Stdio / SSE / HttpStream` | 抽象通信接口，三种具体实现 |
| **插件管理层** | `PluginsLoader` | 插件加载/卸载/热插拔、staging 机制、并发安全 |
| **插件层** | `PluginAPI` + 各插件 `.so/.dll` | C 接口定义、动态链接库、功能实现 |

### 2.3 代码结构

```
mcp_server/
├── CMakeLists.txt                  构建配置（C++20，版本 0.8.0）
├── version.h.in                    版本号模板，CMake 构建时生成 version.h
├── src/
│   ├── main.cpp                    入口：命令行解析、回调配置、启动流程
│   ├── server/
│   │   ├── Server.h                Server 类声明、线程模型、命令注册表
│   │   └── Server.cpp              协议命令实现、请求分发、通知队列
│   ├── transport/
│   │   ├── ITransport.h            传输层抽象接口
│   │   ├── StdioTransport.h/cpp    标准输入输出传输
│   │   ├── SseTransport.h/cpp      Server-Sent Events 传输
│   │   └── HttpStreamTransport.hpp/cpp  HTTP 流式传输
│   ├── loader/
│   │   ├── PluginsLoader.h         PluginEntry 生命周期结构体、PluginsLoader 类
│   │   └── PluginsLoader.cpp       staging 加载、后台监控、三阶段扫描
│   ├── interface/
│   │   ├── ITransport.h            传输层抽象接口
│   │   └── PluginAPI.h             插件 C 接口
│   └── utils/
│       ├── MCPBuilder.h            MCP 响应和通知消息的 JSON 构造器
│       ├── SessionBuilder.h        会话 ID 生成器
│       └── TSingleton.h            线程安全单例模板
├── plugins/                        示例插件
│   ├── weather/                    天气查询工具
│   ├── sleep/                      延迟执行工具
│   ├── code-review/                代码审查工具
│   ├── bacio-quote/                语录工具
│   └── notification/               通知测试工具
├── include/                        第三方头文件
│   ├── httplib.h                   cpp-httplib HTTP 库
│   ├── json.hpp                    nlohmann/json JSON 库
│   ├── base64.hpp                  Base64 编解码
│   └── popl.hpp                    命令行参数解析器
└── libs_tier_01/                   第三方依赖库
    └── aixlog-1.5.0/               日志库
```

---

## 三、核心模块详解

### 3.1 Server（请求调度中心）

#### 设计思路

Server 是整个系统的请求调度中心，采用 **命令模式（Command Pattern）** + **回调覆盖机制** 实现灵活的请求处理。

#### 核心数据结构

```cpp
std::unordered_map<std::string, std::function<json(const json&)>> functionMap;
```

这是一个 **方法名 → 处理函数** 的路由表，在构造函数中注册所有 MCP 协议命令：

| 类别 | 命令 | 说明 |
|------|------|------|
| 生命周期 | `initialize`、`ping` | 握手和心跳 |
| 工具 | `tools/list`、`tools/call` | 列出和调用工具插件 |
| 提示词 | `prompts/list`、`prompts/get` | 列出和获取提示词插件 |
| 资源 | `resources/list`、`resources/read` | 列出和读取资源插件 |
| 通知 | `notifications/initialized` 等 | 各类通知处理 |

#### 回调覆盖机制

```cpp
bool OverrideCallback(const std::string &method, std::function<json(const json &)> function);
```

`main.cpp` 通过此方法将 `tools/list`、`tools/call` 等命令的默认实现替换为从 PluginsLoader 获取插件快照并遍历执行的逻辑。这是**策略模式（Strategy Pattern）**的体现——默认策略在 Server 内部，外部可通过 OverrideCallback 替换策略。

#### 请求处理流程

```
Client 发送 JSON-RPC 请求
    │
    ▼
transport->Read() 读取原始 JSON
    │
    ▼
json::parse() 解析为 JSON 对象
    │
    ▼
HandleRequest() 查找 functionMap
    │
    ├─ 找到 → 调用对应 handler → 返回响应
    └─ 未找到 → 返回 MethodNotFound 错误
    │
    ▼
transport->Write() 发送响应
```

#### 通知队列与 Writer 线程

Server 内部维护一个独立的通知队列和 Writer 线程，实现**请求-通知分离**：

```cpp
std::queue<std::string> notification_queue_;
std::mutex output_mutex_;
std::condition_variable queue_cv_;
std::thread writer_thread_;
```

- 请求响应：在请求处理线程中直接通过 `transport_->Write()` 发送
- 通知消息：先入队，由 Writer 线程异步发送
- 两者共享 `output_mutex_` 保证写入顺序

**为什么需要 Writer 线程？** 因为通知是插件主动发起的（如进度通知），不能阻塞请求处理线程。Writer 线程通过 `condition_variable` 等待，有通知时唤醒发送。

---

### 3.2 Transport（传输协议层）

#### 接口设计

```cpp
class ITransport {
public:
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual std::pair<size_t, std::string> Read() = 0;
    virtual void Write(const std::string& json_data) = 0;
    virtual std::future<std::pair<size_t, std::string>> ReadAsync() = 0;
    virtual std::future<void> WriteAsync(const std::string& json_data) = 0;
    virtual std::string GetName() = 0;
    virtual std::string GetVersion() = 0;
    virtual int GetPort() = 0;
};
```

这是**桥接模式（Bridge Pattern）**的体现——将抽象部分（ITransport 接口）与实现部分（Stdio/SSE/HttpStream）分离，使得两者可以独立变化。

#### 三种传输实现对比

| 特性 | Stdio | SSE | HTTP Stream |
|------|-------|-----|-------------|
| **通信方式** | stdin/stdout | HTTP POST(请求) + GET SSE(推送) | POST(请求+响应) + GET SSE(通知) |
| **端口** | 无 | 8080 | 8080 |
| **适用场景** | 本地客户端（Claude Desktop） | 浏览器/Web 客户端 | REST API 客户端、多客户端 |
| **会话管理** | 无 | session_id 参数 | Mcp-Session-Id 头 |
| **请求-响应** | 同步读写 | POST 入队 + SSE 推送 | POST 同步等待响应 |
| **通知推送** | 直接 stdout | SSE 流推送 | SSE 流推送 |

#### Stdio 传输

最简单的实现，直接从 `stdin` 逐行读取 JSON，向 `stdout` 写入响应。适用于本地进程间通信（如 Claude Desktop 启动子进程）。

```cpp
std::pair<size_t, std::string> Stdio::Read() {
    std::string json_data;
    int c;
    while ((c = std::getc(stdin)) != EOF && c != '\n') {
        json_data += static_cast<char>(c);
    }
    return {json_data.length(), json_data};
}
```

#### SSE 传输

基于 HTTP 长连接的双向通信：

- **客户端 → 服务器**：`POST /messages` 发送请求
- **服务器 → 客户端**：`GET /sse` 建立 SSE 长连接，服务器推送响应和通知
- 首次连接时发送 `endpoint` 事件告知客户端 POST 端点 URL
- 每 15 秒发送 `: ping` 注释作为心跳检测
- 使用 `incoming_messages_` 和 `outgoing_messages_` 两个队列 + 条件变量实现线程间通信

#### HTTP Stream 传输

最完整的实现，基于 MCP 协议的 Streamable HTTP 规范：

- **`POST /mcp`**：客户端发送请求，服务器同步返回响应（通过 promise/future 机制）
- **`GET /mcp`**：建立 SSE 流，接收服务器主动通知
- **`DELETE /mcp`**：终止会话
- 会话管理：`initialize` 请求时生成 `Mcp-Session-Id`，后续请求需携带
- 请求-响应路由：通过 `pending_requests_` map 将响应匹配到对应的 HTTP 请求

```cpp
struct PendingRequest {
    std::promise<std::string> promise;
};
std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_requests_;
```

**核心流程**：
1. HTTP 线程收到 POST 请求 → 创建 `PendingRequest` → 消息入队 → 等待 `future`
2. Server 主循环从队列读取 → 处理请求 → `Write()` 写响应
3. `Write()` 检查是否是响应（有 id + result/error）→ 通过 `promise.set_value()` 唤醒 HTTP 线程
4. HTTP 线程拿到响应 → 返回给客户端

---

### 3.3 PluginsLoader（插件管理与热插拔）

这是本项目最核心、最复杂的模块，涉及动态库加载、并发控制、文件系统监控等多个知识点。

#### PluginEntry 生命周期结构体

```cpp
struct PluginEntry {
    std::string path;              // 原始路径
    std::string stagingPath;       // staging 副本路径
    LibraryHandle handle;          // 动态库句柄
    PluginAPI* instance;           // 插件实例指针
    std::filesystem::file_time_type lastModified;
    std::uintmax_t fileSize;
    PluginAPI* (*createFunc)() = nullptr;
    void (*destroyFunc)(PluginAPI*) = nullptr;

    ~PluginEntry() {
        instance->Shutdown();
        delete instance->notifications;
        destroyFunc(instance);
        dlclose(handle);
        std::filesystem::remove(stagingPath);
    }
};
```

**关键设计**：析构函数定义了完整的清理链——Shutdown → delete notifications → DestroyPlugin → dlclose → 删除 staging 文件。由 `shared_ptr` 引用计数控制析构时机，确保正在被请求使用的插件不会被提前卸载。

#### Staging 机制

**问题**：为什么不能直接 `dlopen` 原始插件路径？

**答案**：插件内部使用 `static PluginAPI plugin` 静态对象。如果对同一路径多次 `dlopen`，操作系统会复用已加载的模块而不是加载新文件，导致热更新失败。

**解决方案**：将插件文件复制到 `.staging/` 目录，文件名带纳秒时间戳（如 `libweather_1719000001.so`），确保每次加载都是独立的模块实例。

```
原始文件: plugins/libweather.so
    │
    ▼ CopyToStaging()
staging 副本: plugins/.staging/libweather_1719000001.so
    │
    ▼ dlopen()
独立模块实例
```

#### 指纹校验

在复制前后各读取一次源文件的 mtime 和 size。如果不一致，说明复制期间文件被构建系统覆盖了，本次加载的内容可能不完整，丢弃 staging 副本，等待下一轮扫描重试。

```cpp
auto preSnapshotMtime = std::filesystem::last_write_time(path);
auto preSnapshotSize = std::filesystem::file_size(path);
// ... 复制到 staging ...
auto postMtime = std::filesystem::last_write_time(path);
auto postSize = std::filesystem::file_size(path);
if (postMtime != preSnapshotMtime || postSize != preSnapshotSize) {
    // 源文件在复制期间被修改，丢弃本次加载
}
```

#### 三阶段扫描算法

这是热插拔的核心算法，将扫描分为三个阶段，最小化锁持有时间：

**第一阶段（读锁）**：收集差异

```cpp
std::shared_lock lock(m_pluginsMutex);
// 对比磁盘文件和已加载插件列表
// 生成 toUpdate / toDelete / toAdd 三个列表
```

**第二阶段（无锁）**：创建新实例

```cpp
// 释放所有锁，执行耗时的文件 I/O 和 dlopen
// 对 toUpdate 和 toAdd 中的路径执行完整加载流程
```

**第三阶段（写锁）**：原子切换

```cpp
std::unique_lock lock(m_pluginsMutex);
// 只做指针交换/插入/删除，不执行耗时操作
// 旧实例移入 oldEntries，锁外释放
```

**设计精髓**：第二阶段是耗时操作（文件复制、dlopen、Initialize），完全不持锁，不影响正在处理的请求。第三阶段只做指针操作，写锁持有时间极短。

#### 失败重试策略

维护 `m_failedPlugins` 缓存，记录加载失败插件的文件指纹（mtime + size）：

- 文件未变化 → 跳过，不重复尝试
- 文件已变化 → 清除缓存，重新尝试
- `kSourceChangedDuringCopy`（复制期间源文件被修改）→ 不记入缓存，下轮自动重试

---

### 3.4 PluginAPI（插件接口）

#### C 接口设计

```cpp
extern "C" {
    PLUGIN_API PluginAPI* CreatePlugin();
    PLUGIN_API void DestroyPlugin(PluginAPI*);
}
```

**为什么用 C 接口？**

1. **ABI 稳定性**：C 接口有稳定的二进制接口，不同编译器/版本的 C++ 编译的动态库可以互操作
2. **名称修饰（Name Mangling）**：C++ 编译器会对函数名进行修饰，`extern "C"` 禁止修饰，确保符号名可预测
3. **dlsym 查找**：通过字符串查找符号需要名称可预测

#### PluginAPI 结构体（函数指针表）

```cpp
typedef struct {
    const char* (*GetName)();
    const char* (*GetVersion)();
    PluginType (*GetType)();
    int (*Initialize)();
    char* (*HandleRequest)(const char* request);
    void (*Shutdown)();
    int (*GetToolCount)();
    const PluginTool* (*GetTool)(int index);
    int (*GetPromptCount)();
    const PluginPrompt* (*GetPrompt)(int index);
    int (*GetResourceCount)();
    const PluginResource* (*GetResource)(int index);
    NotificationSystem* notifications;
} PluginAPI;
```

这是**C 语言面向对象**的经典手法——用函数指针表模拟虚函数表（vtable），实现多态。

#### 跨平台动态库导出

```cpp
#ifdef _WIN32
#define PLUGIN_API __declspec(dllexport)
#else
#define PLUGIN_API __attribute__((visibility("default")))
#endif
```

#### 插件类型

```cpp
typedef enum {
    PLUGIN_TYPE_TOOLS = 0,
    PLUGIN_TYPE_PROMPTS = 1,
    PLUGIN_TYPE_RESOURCES = 2
} PluginType;
```

对应 MCP 协议的三种能力：工具（可调用的函数）、提示词（预定义的提示模板）、资源（可读取的数据源）。

---

### 3.5 工具类

#### MCPBuilder（MCP 消息构造器）

提供静态方法构造 MCP 协议的各类消息：

```cpp
class MCPBuilder {
public:
    enum ErrorCode {
        ParseError = -32700,      // JSON-RPC 标准错误码
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams = -32602,
        InternalError = -32603
    };

    static json Response(json request);           // 构造响应
    static json Error(ErrorCode, id, message);    // 构造错误
    static json TextContent(text);                // 文本内容
    static json ImageContent(data, mimeType);     // 图片内容（Base64）
    static json AudioContent(data, mimeType);     // 音频内容（Base64）
    static json ResourceText(uri, mime, text);    // 资源文本
    static json NotificationLog(level, data);     // 日志通知
    static json NotificationProgress(...);        // 进度通知
    static json NotificationToolsListChanged();   // 工具列表变更通知
    // ...
};
```

#### SessionBuilder（会话 ID 生成器）

```cpp
static std::string GenerateUniqueSessionID() {
    auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<uint32_t> dis;
    std::stringstream ss;
    ss << std::hex << timestamp << "-" << dis(gen);
    return ss.str();
}
```

使用**时间戳 + 随机数**组合生成唯一会话 ID，格式如 `1a2b3c4d5e6f-12345678`。

#### TSingleton（线程安全单例模板）

```cpp
template <typename T>
class TSingleton {
    static T& GetInstance() {
        std::call_once(initFlag, []() {
            instance.reset(new T());
        });
        return *instance;
    }
};
```

使用 `std::call_once` + `std::once_flag` 实现线程安全的延迟初始化，比双重检查锁定（DCLP）更简洁安全。

---

## 四、线程模型

### 4.1 线程概览

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   主线程      │     │  Watcher线程  │     │  Writer线程   │
│  (请求处理)   │     │ (目录扫描)    │     │ (通知发送)    │
├──────────────┤     ├──────────────┤     ├──────────────┤
│              │     │              │     │              │
│ GetPlugins   │     │ ScanFor      │     │ WriterLoop   │
│ Snapshot()   │     │ Changes()    │     │              │
│   读锁(短暂) │     │  读锁(分析)   │     │ 等待通知队列  │
│              │     │  无锁(加载)   │     │              │
│ HandleReq()  │     │  写锁(切换)   │     │ transport    │
│   (无锁)     │     │              │     │ ->Write()    │
│              │     │ 回调通知     │     │              │
└──────────────┘     └──────────────┘     └──────────────┘
```

对于 SSE/HTTP Stream 传输，还有额外的 HTTP 服务器线程。

### 4.2 并发控制机制

| 共享数据 | 保护方式 | 说明 |
|----------|----------|------|
| `m_plugins` | `std::shared_mutex` | 请求线程读锁、watcher 线程写锁 |
| `notification_queue_` | `std::mutex` + `std::condition_variable` | Writer 线程消费 |
| `incoming_messages_` | `std::mutex` + `std::condition_variable` | SSE/HTTP 传输消息队列 |
| `outgoing_messages_` | `std::mutex` + `std::condition_variable` | SSE 推送队列 |
| `m_watching` | `std::atomic<bool>` | 控制 watcher 线程退出 |
| `isStopping_` | `std::atomic<bool>` | 控制请求主循环退出 |
| `server_running_` | `std::atomic<bool>` | 控制 HTTP 服务器状态 |
| `client_connected_` | `std::atomic<bool>` | SSE 客户端连接状态 |

### 4.3 读写锁（shared_mutex）的使用

```cpp
// 读操作（请求线程）：允许多个请求同时读取
std::shared_lock lock(m_pluginsMutex);
return m_plugins;  // 返回 vector<shared_ptr> 的拷贝

// 写操作（watcher 线程）：独占访问
std::unique_lock lock(m_pluginsMutex);
// 修改 m_plugins
```

**关键**：`GetPluginsSnapshot()` 返回的是 `vector<shared_ptr>` 的拷贝，读锁只在拷贝期间持有（微秒级），之后请求线程在完全无锁状态下执行插件代码。

---

## 五、完整知识点清单

### 5.1 C++ 语言特性

| 知识点 | 项目中的应用 | 面试重点 |
|--------|-------------|----------|
| **C++20 标准** | CMake 中 `CMAKE_CXX_STANDARD 20` | 了解 C++20 新特性（concepts、ranges、coroutines 等） |
| **智能指针** | `shared_ptr<PluginEntry>`、`unique_ptr<httplib::Server>` | 所有权语义、引用计数、循环引用 |
| **RAII** | `PluginEntry` 析构函数自动清理资源 | 资源获取即初始化，异常安全 |
| **移动语义** | `std::move(entry)`、`std::move(function)` | 右值引用、完美转发、移动构造/赋值 |
| **delete 关键字** | `Server(const Server&) = delete` | 禁止拷贝/移动的场景和原因 |
| **lambda 表达式** | `functionMap` 中的命令注册、回调配置 | 捕获列表、闭包、`std::function` |
| **std::function** | 命令路由表、回调函数类型擦除 | 类型擦除、性能开销、与函数指针对比 |
| **std::atomic** | `isStopping_`、`m_watching`、`server_running_` | 内存序（memory_order）、CAS 操作 |
| **std::shared_mutex** | `m_pluginsMutex` 保护插件列表 | 读写锁、适用场景、与 mutex 对比 |
| **std::condition_variable** | 通知队列、消息队列的等待/唤醒 | 虚假唤醒、wait/wait_for/notify_one/notify_all |
| **std::call_once** | TSingleton 中的线程安全初始化 | 与双重检查锁定的对比 |
| **std::future/promise** | HTTP Stream 中的请求-响应匹配 | 异步编程模型、shared_future |
| **std::thread** | Writer 线程、Watcher 线程、HTTP 服务器线程 | join/detach、线程安全 |
| **std::filesystem** | 文件遍历、mtime 读取、staging 管理 | C++17 文件系统库 |
| **extern "C"** | 插件接口的 C 链接 | 名称修饰、ABI 兼容性 |
| **模板** | `TSingleton<T>` 单例模板 | 模板实例化、CRTP |
| **结构化绑定** | `auto [length, json_string] = transport->Read()` | C++17 结构化绑定 |
| **volatile sig_atomic_t** | 信号处理中的停止标志 | 信号安全函数、async-signal-safe |

### 5.2 设计模式

| 设计模式 | 项目中的应用 | 面试话术 |
|----------|-------------|----------|
| **策略模式** | `OverrideCallback()` 替换默认命令处理 | "通过回调覆盖实现策略的运行时替换" |
| **命令模式** | `functionMap` 方法名→处理函数映射 | "将请求封装为对象，支持命令路由和覆盖" |
| **桥接模式** | `ITransport` 接口与三种实现分离 | "将抽象与实现解耦，使两者可独立变化" |
| **观察者模式** | `OnPluginsChanged` 回调通知客户端 | "插件变化时通知订阅者刷新能力列表" |
| **模板方法模式** | `PluginAPI` 定义接口骨架，插件实现细节 | "定义算法骨架，子类实现具体步骤" |
| **单例模式** | `TSingleton<T>` 线程安全单例 | "call_once 实现线程安全的延迟初始化" |
| **生产者-消费者** | 通知队列 + Writer 线程 | "解耦通知生产和发送，避免阻塞请求处理" |
| **快照模式** | `GetPluginsSnapshot()` 返回插件列表拷贝 | "通过拷贝实现无锁读取，引用计数延迟释放" |
| **代理模式** | staging 副本作为原始插件的代理 | "通过副本实现独立加载，避免 dlopen 缓存问题" |

### 5.3 操作系统与系统编程

| 知识点 | 项目中的应用 | 面试重点 |
|--------|-------------|----------|
| **动态库加载** | `dlopen/dlsym/dlclose`（Linux）、`LoadLibrary/GetProcAddress/FreeLibrary`（Windows） | 符号解析、RTLD_LAZY vs RTLD_NOW |
| **进程间通信** | Stdio 传输使用 stdin/stdout | 管道、重定向、进程启动 |
| **网络编程** | SSE/HTTP Stream 基于 httplib | HTTP 协议、长连接、SSE 规范 |
| **信号处理** | `SIGINT` → `stop_handler()` | 信号安全函数、volatile sig_atomic_t |
| **文件系统监控** | 定时扫描 + mtime/size 比对 | inotify/FSEvents 对比、轮询方案的优劣 |
| **CORS** | SSE/HTTP 传输设置 CORS 头 | 跨域资源共享、预检请求 |
| **SSE（Server-Sent Events）** | 服务器向客户端推送消息 | 与 WebSocket 对比、断线重连、事件格式 |

### 5.4 并发编程

| 知识点 | 项目中的应用 | 面试重点 |
|--------|-------------|----------|
| **读写锁** | `shared_mutex` 保护插件列表 | 读写锁 vs 互斥锁的性能对比 |
| **条件变量** | 通知队列、消息队列 | wait/wait_for 的区别、虚假唤醒 |
| **原子操作** | `atomic<bool>` 控制线程退出 | memory_order、原子性保证 |
| **引用计数** | `shared_ptr` 延迟释放旧插件 | 循环引用、weak_ptr |
| **线程安全队列** | `notification_queue_`、`incoming_messages_` | 生产者-消费者模式 |
| **锁粒度优化** | 三阶段扫描最小化锁持有时间 | 锁粒度与性能的权衡 |
| **双重检查** | `isSyncCleaned_.exchange(true)` 确保清理只执行一次 | 原子操作实现一次性初始化 |

### 5.5 协议与规范

| 知识点 | 项目中的应用 | 面试重点 |
|--------|-------------|----------|
| **JSON-RPC 2.0** | MCP 协议基于 JSON-RPC 2.0 | 请求/响应/通知格式、错误码 |
| **MCP 协议** | 整个项目的核心协议 | Tools/Prompts/Resources 三大能力、生命周期 |
| **HTTP/1.1** | SSE 和 HTTP Stream 传输 | 长连接、Content-Type、状态码 |
| **SSE 规范** | 服务器推送事件 | `text/event-stream`、事件格式、重连机制 |
| **JSON Schema** | 插件工具的 `inputSchema` | 参数验证、类型系统 |

### 5.6 构建系统与工具链

| 知识点 | 项目中的应用 | 面试重点 |
|--------|-------------|----------|
| **CMake** | 项目构建配置 | target、find_package、configure_file |
| **版本号管理** | `version.h.in` + `configure_file()` | CMake 模板变量替换 |
| **跨平台编译** | 条件编译处理 Windows/Linux/macOS 差异 | 预处理器宏、平台检测 |
| **静态链接** | `-static-libgcc -static-libstdc++` | 运行时依赖管理 |
| **动态库导出** | `__declspec(dllexport)` / `__attribute__((visibility))` | 符号可见性、导出控制 |

---

## 六、面试高频问题与回答

### 6.1 架构设计类

**Q：为什么选择插件架构？有什么好处？**

A：插件架构将核心服务器与功能实现解耦，带来三大好处：
1. **可扩展性**：新增功能只需编写新插件（一个 .cpp 文件编译为 .so/.dll），无需修改服务器代码
2. **隔离性**：插件崩溃不会影响服务器核心，每个插件是独立的动态库
3. **热插拔**：运行时可以增删改插件，无需重启服务器，对 AI 客户端透明

**Q：为什么传输层要用接口抽象？**

A：MCP 协议支持多种传输方式，不同客户端有不同的通信需求：
- Claude Desktop 通过子进程 stdin/stdout 通信（Stdio）
- Web 客户端需要 HTTP 长连接（SSE）
- REST API 客户端需要请求-响应模式（HTTP Stream）

通过 `ITransport` 接口抽象，Server 不需要关心底层通信方式，符合**依赖倒置原则**。新增传输方式只需实现 `ITransport` 接口。

**Q：三阶段扫描算法为什么要分三个阶段？**

A：核心目标是**最小化锁持有时间**，最大化并发性能：
- 第一阶段（读锁）：只做轻量的路径对比，生成待处理列表
- 第二阶段（无锁）：执行耗时的文件 I/O 和 dlopen，不阻塞请求线程
- 第三阶段（写锁）：只做指针交换，微秒级完成

如果全程持锁，加载一个插件可能需要几十毫秒（文件复制 + dlopen），期间所有请求都会被阻塞。

### 6.2 并发编程类

**Q：shared_ptr 的引用计数如何保证线程安全？**

A：`shared_ptr` 的引用计数本身是原子操作（通过 `std::atomic` 实现），所以：
- 多个线程可以安全地拷贝 `shared_ptr`（引用计数增减是原子的）
- 但通过 `shared_ptr` 访问指向的对象**不是**线程安全的

在本项目中，`GetPluginsSnapshot()` 返回 `vector<shared_ptr>` 的拷贝，请求线程持有快照后，即使 watcher 线程替换了插件列表中的 `shared_ptr`，旧插件的引用计数不为零，不会被析构。这实现了**延迟释放**——旧插件在最后一个请求完成后才被卸载。

**Q：为什么用 shared_mutex 而不是 mutex？**

A：`shared_mutex` 允许多个读者同时访问，适合**读多写少**的场景。本项目中：
- 请求线程频繁读取插件列表（每次请求都读）
- watcher 线程每 5 秒才写一次

如果用 `mutex`，所有读操作会串行化，成为性能瓶颈。`shared_mutex` 让多个请求可以并发读取，只有写操作才需要独占锁。

**Q：condition_variable 的 wait_for 为什么用 200ms？**

A：这是 SSE/HTTP Stream 传输中的设计选择：
- 不能用 `wait()`（无限等待），因为需要定期检查连接状态和发送心跳
- 不能太短（如 1ms），因为会导致 CPU 空转
- 200ms 是一个平衡点：足够快地响应消息，又不会过度消耗 CPU

### 6.3 动态库与插件类

**Q：为什么插件要用 extern "C" 导出？**

A：C++ 编译器会对函数名进行**名称修饰（Name Mangling）**，不同编译器、不同版本的修饰规则不同。`extern "C"` 禁止名称修饰，确保：
1. `dlsym` 可以通过字符串 `"CreatePlugin"` 找到符号
2. 不同编译器编译的插件和宿主可以互操作（ABI 兼容）

**Q：staging 机制解决了什么问题？**

A：解决了 `dlopen` 的**缓存问题**。操作系统对同一路径的 `dlopen` 调用会返回已加载的模块句柄，不会重新加载文件。这意味着即使磁盘上的 .so 文件已更新，`dlopen` 同一路径仍会得到旧版本。

通过将文件复制到带纳秒时间戳的唯一路径再加载，每次都是全新的模块实例，确保热更新生效。

**Q：插件更新时如何保证正在处理的请求不受影响？**

A：通过 `shared_ptr` 引用计数实现**延迟释放**：
1. 请求线程通过 `GetPluginsSnapshot()` 获取插件列表拷贝，持有 `shared_ptr`
2. watcher 线程替换插件时，旧 `shared_ptr` 从列表移出，但请求线程仍持有引用
3. 旧插件的引用计数 > 0，不会触发析构
4. 请求完成后，`shared_ptr` 离开作用域，引用计数归零，旧插件才被卸载

### 6.4 网络编程类

**Q：SSE 和 WebSocket 有什么区别？为什么选 SSE？**

A：
| 特性 | SSE | WebSocket |
|------|-----|-----------|
| 方向 | 服务器→客户端单向 | 双向 |
| 协议 | HTTP | 独立协议（ws://） |
| 重连 | 浏览器自动重连 | 需手动实现 |
| 数据格式 | 文本 | 文本/二进制 |

MCP 协议的通信模式是：客户端发请求，服务器回响应 + 推送通知。这天然适合 SSE——请求用 HTTP POST，推送用 SSE 流。不需要 WebSocket 的双向能力。

**Q：HTTP Stream 传输如何实现请求-响应匹配？**

A：通过 `promise/future` 机制：
1. HTTP 线程收到 POST 请求，解析出 `id`，创建 `PendingRequest{promise}`
2. 请求消息入队，Server 主循环读取处理
3. Server 调用 `Write()` 写响应时，检查 JSON 中的 `id`，匹配到 `pending_requests_` 中的条目
4. 通过 `promise.set_value()` 唤醒等待的 HTTP 线程
5. HTTP 线程拿到响应，返回给客户端

### 6.5 C++ 语言细节类

**Q：delete 拷贝构造函数的意义？**

A：`Server` 类禁止拷贝和移动，因为：
1. 包含 `std::thread` 成员，线程不可拷贝
2. 包含 `std::mutex` 成员，互斥锁不可拷贝
3. 包含 `std::atomic` 成员，原子变量不可拷贝
4. 语义上 Server 是单例，不应被拷贝

**Q：volatile sig_atomic_t 和 atomic<bool> 有什么区别？**

A：
- `volatile sig_atomic_t`：用于信号处理函数中，保证对变量的读写是原子的（相对于信号），是 C 标准保证的信号安全类型
- `atomic<bool>`：C++11 的原子类型，提供更强的多线程保证（各种 memory_order）

在信号处理函数中只能使用**异步信号安全**（async-signal-safe）的操作，`atomic<bool>` 的 `store()` 在大多数实现上是信号安全的，但标准不保证。所以用 `volatile sig_atomic_t` 更安全。

**Q：std::call_once 和双重检查锁定有什么区别？**

A：
- `std::call_once`：C++11 标准 guarantee 线程安全，代码简洁，推荐使用
- 双重检查锁定（DCLP）：容易写错（内存屏障问题），C++11 之前常用

`TSingleton` 使用 `call_once`，更安全更简洁。

### 6.6 项目经验类

**Q：如果让你重新设计这个项目，你会做哪些改进？**

A：
1. **插件沙箱**：当前插件与宿主在同一进程空间，一个插件崩溃会导致整个服务器崩溃。可以引入进程隔离或异常捕获机制
2. **配置热加载**：当前只支持插件热插拔，服务器配置（端口、日志级别等）修改需要重启
3. **多会话支持**：HTTP Stream 当前只支持单会话，可以扩展为多会话并发
4. **插件依赖管理**：当前插件之间没有依赖关系，可以引入依赖声明和加载顺序
5. **更高效的文件监控**：用 inotify（Linux）/ FSEvents（macOS）/ ReadDirectoryChangesW（Windows）替代轮询
6. **插件版本协商**：当前没有版本兼容性检查，可以引入 API 版本号

**Q：项目中遇到的最大技术挑战是什么？**

A：**热插拔的并发安全**。核心矛盾是：
- 请求线程需要稳定地访问插件（不能正在调用时插件被卸载）
- watcher 线程需要及时更新插件（不能长时间持锁阻塞请求）

解决方案是三阶段扫描 + `shared_ptr` 快照 + 延迟释放的组合：
- 三阶段扫描最小化锁持有时间
- `shared_ptr` 快照让请求线程在无锁状态下执行
- 延迟释放确保正在使用的插件不会被提前卸载

---

## 七、数据流图

### 7.1 请求处理完整流程

```
客户端发送 JSON-RPC 请求
    │
    ▼
Transport.Read() 读取原始 JSON 字符串
    │
    ├─ Stdio: stdin 逐行读取
    ├─ SSE: POST /messages → incoming_messages_ 队列
    └─ HTTP: POST /mcp → incoming_messages_ 队列 + pending_requests_ 注册
    │
    ▼
Server.Connect() 主循环
    │
    ▼
json::parse() 解析为 JSON 对象
    │
    ▼
HandleRequest() 查找 functionMap
    │
    ├─ 找到 handler → 调用处理函数
    │   │
    │   ├─ 默认 handler（如 InitializeCmd）
    │   └─ OverrideCallback 覆盖的 handler（如 tools/list）
    │       │
    │       ▼
    │       loader->GetPluginsSnapshot() 获取插件快照
    │       │
    │       ▼
    │       遍历插件 → plugin->instance->HandleRequest()
    │       │
    │       ▼
    │       构造响应 JSON
    │
    └─ 未找到 → 返回 MethodNotFound 错误
    │
    ▼
Transport.Write() 发送响应
    │
    ├─ Stdio: stdout 直接输出
    ├─ SSE: outgoing_messages_ 队列 → SSE 流推送
    └─ HTTP: promise.set_value() → HTTP 线程返回响应
```

### 7.2 热插拔完整流程

```
Watcher 线程每 5 秒执行 ScanForChanges()
    │
    ▼
第一阶段（读锁）：收集 toUpdate / toDelete / toAdd
    │
    ▼
第二阶段（无锁）：对每个路径执行 CreatePluginInstance()
    │
    ├─ 采集文件指纹（mtime + size）
    ├─ CopyToStaging() 复制到 .staging/ 目录
    ├─ 校验源文件未被修改
    ├─ dlopen() 加载 staging 副本
    ├─ dlsym() 获取 CreatePlugin/DestroyPlugin
    ├─ CreatePlugin() 创建实例
    ├─ Initialize() 初始化
    └─ OnPluginLoaded 回调（设置通知系统）
    │
    ▼
第三阶段（写锁）：原子切换
    │
    ├─ 更新：指针交换，旧实例移入 oldEntries
    ├─ 删除：从列表移除，移入 oldEntries
    └─ 新增：加入列表
    │
    ▼
锁外收尾
    │
    ├─ oldEntries.clear() → 旧插件延迟释放
    ├─ 清理 m_failedPlugins
    └─ OnPluginsChanged 回调 → 发送 list_changed 通知
        │
        ▼
        Server.SendNotification() → notification_queue_
            │
            ▼
            Writer 线程 → transport_->Write() → 客户端
```

---

## 八、关键代码片段解析

### 8.1 命令路由与回调覆盖

```cpp
// Server 构造函数：注册默认命令
Server::Server() {
    functionMap = {
        {"initialize", [this](const json& req) { return this->InitializeCmd(req); }},
        {"tools/list", [this](const json& req) { return this->ToolsListCmd(req); }},
        // ...
    };
}

// main.cpp：覆盖默认实现
server->OverrideCallback("tools/list", [](const json& request) {
    auto plugins = loader->GetPluginsSnapshot();
    // 遍历插件，构造工具列表响应
    return response;
});
```

**设计精髓**：Server 提供默认的空实现，main.cpp 通过 OverrideCallback 注入与 PluginsLoader 耦合的逻辑。Server 本身不知道 PluginsLoader 的存在，实现了**控制反转（IoC）**。

### 8.2 快照访问模式

```cpp
// PluginsLoader：读锁下拷贝 vector
std::vector<std::shared_ptr<PluginEntry>> PluginsLoader::GetPluginsSnapshot() const {
    std::shared_lock lock(m_pluginsMutex);
    return m_plugins;  // 拷贝 vector<shared_ptr>，引用计数 +1
}

// 请求线程：无锁使用快照
auto plugins = loader->GetPluginsSnapshot();  // 读锁，微秒级
for (const auto& plugin : plugins) {
    plugin->instance->HandleRequest(...);      // 无锁，可以是耗时操作
}
// plugins 离开作用域，引用计数 -1
```

### 8.3 信号安全停止

```cpp
// 信号处理函数：只设置原子标志
volatile sig_atomic_t g_stopRequested = 0;

void stop_handler(sig_atomic_t s) {
    g_stopRequested = 1;          // 信号安全
    if (server) {
        server->RequestStop();    // 只设置 atomic<bool>
    }
}

// 主循环：检测标志后退出
while (!isStopping_) {
    auto [length, json_string] = transport->Read();
    if (isStopping_) break;
    // ...
}

// main()：Connect 返回后执行清理
loader->StopWatching();
loader->UnloadPlugins();
server->Stop();
```

---

## 九、第三方库

| 库 | 用途 | 特点 |
|----|------|------|
| **cpp-httplib** | HTTP 服务器/客户端 | Header-only，单文件，支持 SSE |
| **nlohmann/json** | JSON 解析与构造 | Header-only，API 友好，支持 ordered_json |
| **aixlog** | 日志记录 | 轻量级，支持文件输出 |
| **popl** | 命令行参数解析 | Header-only，类似 getopt |
| **base64** | Base64 编解码 | 用于图片/音频内容的编码 |

---

## 十、面试速记卡

### 一句话概括项目

> "一个基于 C++20 的 MCP 协议服务器，采用插件架构支持热插拔，通过策略模式实现命令路由，读写锁+快照实现并发安全的插件访问，支持 Stdio/SSE/HTTP Stream 三种传输协议。"

### 核心技术关键词

`C++20` `JSON-RPC 2.0` `MCP协议` `插件架构` `热插拔` `dlopen` `shared_mutex` `shared_ptr` `condition_variable` `SSE` `staging机制` `三阶段扫描` `extern "C"` `RAII` `策略模式` `桥接模式` `生产者-消费者`

### 最可能被追问的三个问题

1. **热插拔如何保证并发安全？** → 三阶段扫描 + shared_ptr 快照 + 延迟释放
2. **为什么不直接 dlopen 原始路径？** → 操作系统缓存问题，staging 副本确保独立模块
3. **如何实现请求-响应匹配？** → promise/future + pending_requests map

### 项目亮点（面试中主动提及）

1. **零停机热更新**：插件更新无需重启，对客户端透明
2. **细粒度并发控制**：读写锁 + 快照模式，请求处理完全无锁
3. **协议完整性**：完整实现 MCP 规范的 Tools/Prompts/Resources 三大能力
4. **跨平台**：Windows/Linux/macOS 三平台支持
5. **延迟释放机制**：shared_ptr 引用计数确保正在使用的插件不被提前卸载