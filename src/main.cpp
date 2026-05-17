#include "HttpStreamTransport.hpp"
#include "SseTransport.h"
#include "StdioTransport.h"
#include "aixlog.hpp"
#include "httplib.h"
#include "json.hpp"
#include "loader/PluginsLoader.h"
#include "popl.hpp"
#include "server/Server.h"
#include "utils/MCPBuilder.h"
#include "version.h"
#include <csignal>

// 【命名空间说明】
// 命名空间popl：一个轻量级的 C++ 命令行参数解析库
// 命名空间vx：是项目的顶层命名空间，包含了所有核心组件和功能模块。
// 命名空间vx::mcp：是vx命名空间下专门用于MCP协议相关实现的子命名空间，包含了Server、PluginsLoader等与MCP协议直接相关的类。

using namespace popl;

// ?[两大对象] 创建Server、PluginsLoader的“全局对象”(这里只声明，后面会在main中初始化)
// ! 为啥设为全局变量？————以便在信号处理函数和其他回调中访问和控制服务器行为
std::shared_ptr<vx::mcp::Server> server;
std::shared_ptr<vx::mcp::PluginsLoader> loader;

// ?[全局变量] g_stopRequested：一个全局原子标志，用于在接收到 SIGINT 信号（如 Ctrl+C）时通知主循环安全地退出。
// todo:初始=0，后续在stop_handler函数中设置该标志=1。在主循环中定期检查它的状态，一旦发现被设置，就开始优雅地关闭服务器，释放资源。
volatile sig_atomic_t g_stopRequested = 0;
// [数据类型]：volatile sig_atomic_t类型————能够确保在信号处理函数中修改时的线程安全和可见性。
// -volatile：告诉编译器这个变量可能在程序的任何地方被异步修改，禁止对它进行某些优化，确保每次访问都直接从内存读取，而不是使用寄存器缓存。
// -sig_atomic_t：这是一个特殊的整数类型，保证在访问时不会被中断（即原子操作），适合在信号处理函数中使用，避免竞态条件和数据损坏。
// [默认值]：初始化为0，表示正常运行状态；当接收到 SIGINT 信号时，信号处理函数会将其设置为1，表示请求停止。
// [作用]：主循环会定期检查这个标志，一旦发现被设置，就会开始优雅地关闭服务器。
// [注]
// 信号处理函数只设置原子标志，不执行任何非 async-signal-safe 操作。
// 对于 stdio 传输，SIGINT 会中断阻塞的 read() 系统调用使 Connect 循环退出。
// 对于 HTTP/SSE 传输，Connect 循环会在下次迭代检测到 isStopping_ 后退出。

// ?[全局变量] 定义全局结构体变量-notificationState：
// todo:一个全局结构体实例，包含一个互斥锁，用于保护服务器发送通知时的线程安全。
struct NotificationState
{
  std::mutex serverNotificationMutex;
};
NotificationState notificationState; // 后续会在“插件回调”中使用这个互斥锁，确保在多线程环境下向服务器发送通知时不会发生竞态条件。

// ?[函数] stop_handler-信号处理函数
// todo:用于替换默认的 SIGINT 处理行为，使得在接收到 Ctrl+C 时能够优雅地关闭服务器，而不是直接终止进程。
// todo:回顾“达内Webserver”
// 告诉内核："如果用户按了 Ctrl+C（产生SIGINT），不要直接杀死进程，而是执行我的 stop_handler"。
// 参数：s-接收到的信号编号，通常是 SIGINT（由用户 Ctrl+C 触发）
void stop_handler(sig_atomic_t s)
{
  g_stopRequested = 1;
  if (server)
  {
    server->RequestStop();
  }
}

// ?[函数] ClientNotificationCallbackImpl-插件通知回调实现
// todo:这是一个全局函数，作为插件发送通知到 MCP 客户端的回调实现。
// 当插件 plugin 需要向 MCP-client发送通知时，会调用这个函数。
// 内部用到了上面定义的notificationState.serverNotificationMutex互斥锁，确保在多线程环境下向服务器发送通知时的线程安全。
void ClientNotificationCallbackImpl(const char *pluginName,
                                    const char *notification)
{
  std::lock_guard<std::mutex> lock(notificationState.serverNotificationMutex);
  if (server && server->IsValid())
  {
    server->SendNotification(pluginName, notification);
  }
}

// 【程序入口】负责解析启动参数、构建传输层、加载插件、
// 注册 MCP 请求处理器，并进入主消息循环。
// 这里的设计原则是：
// 1.启动参数解析和传输层构建只做一次，且在程序最开始就完成，确保后续流程有明确的通信通道。
// 2. 插件加载和 MCP 请求处理器注册放在一起，形成清晰的“能力挂接”逻辑。
// 3. 主消息循环在 Server 内部实现，main 函数只负责启动和优雅关闭，保持简洁。

int main(int argc, char **argv)
{
  // !==============================================================================
  // !【阶段一】前期准备：声明配置变量、核心对象初始化、信号处理函数注册、命令行参数解析、日志系统初始化
  // !==============================================================================

  //============================================================================================
  // 定义各种配置变量和核心对象
  // todo:[变量定义和处理逻辑de"解耦"]
  // todo:把main函数需要的各种变量和对象————在最开始就定义出来，后续只写各种逻辑。
  //============================================================================================

  // ?[定义变量]-服务器相关的变量（后续会根据“命令行参数”进行赋值）
  std::string name;              // 服务器名称，默认 "mcp-server"，可以通过命令行参数覆盖
  std::string plugins_directory; // 插件目录，默认"./plugins"，服务启动后会从这个目录加载插件，并监控其变化
  std::string logs_directory;    // 日志目录，默认 "./logs"
  bool verbose;                  // 是否启用详细日志（bool类型），默认false，可以通过命令行参数“--verbose”启用

  // ?[初始化：传输层基类指针]
  // transport-传输层接口指针：后续会根据命令行参数选择具体的传输实现
  // （SSE、HTTP Stream 或 Stdio）
  std::shared_ptr<vx::ITransport> transport;

  // ?[初始化：两大核心对象]————PluginsLoader/Server（以智能指针形式实例化）
  // 1.loader-PluginsLoader类：插件加载器，后续会用它来加载插件并监控插件目录的变化，实现热更新功能。
  // 2.server-Server类：MCP协议服务器————最核心的组件，负责处理来自客户端的请求，并通过注册的回调函数把请求分发给插件处理。
  loader = std::make_shared<vx::mcp::PluginsLoader>();
  server = std::make_shared<vx::mcp::Server>();
  // 注：Server 内部会维护一个指向 ITransport 的指针

  //============================================================================================
  // 注册信号处理函数
  // 这里只做"请求停止"这件事，真正的资源回收仍在 main 末尾统一执行，
  // 这样可以避免在信号上下文里做复杂操作。
  // todo:[翻译为]-按下Ctrl+C时 → 产生SIGINT信号后 → 处理逻辑改为：调用stop_handler函数来处理这个信号。
  //============================================================================================
  signal(SIGINT, stop_handler);

  //============================================================================================
  // 配置命令行参数
  // 这些参数决定服务名、插件目录、日志目录，以及底层采用哪种传输方式。
  //============================================================================================
  OptionParser op("Allowed options");
  auto help_option = op.add<Switch>("", "help", "produce help message");
  auto name_option = op.add<Value<std::string>>(
      "n", "name", "the name of the server", "mcp-server");
  auto plugins_directory_option = op.add<Value<std::string>>(
      "p", "plugins", "the directory where to load the plugins", "./plugins");
  auto logs_directory_option = op.add<Value<std::string>>(
      "l", "logs", "the directory where to store the logs", "./logs");
  auto verbose_option =
      op.add<Value<bool>>("v", "verbose", "enable verbose", verbose);
  auto use_sse_server = op.add<Switch>("s", "sse", "start as sse server");
  auto use_httpstream_server =
      op.add<Switch>("t", "httpstream", "start as http stream server");
  name_option->assign_to(&name);
  plugins_directory_option->assign_to(&plugins_directory);
  logs_directory_option->assign_to(&logs_directory);
  verbose_option->assign_to(&verbose);

  //============================================================================================
  // 解析命令行参数
  // 如果传入 --help，则直接打印帮助并退出；如果参数非法，则在这里提前失败。
  //============================================================================================
  try
  {
    op.parse(argc, argv);
    if (help_option->count() == 1)
    {
      std::cout << op << std::endl;
      return 0;
    }
  }
  catch (const popl::invalid_option &e)
  {
    std::cerr << "Invalid Option Exception: " << e.what() << std::endl;
    return -1;
  }
  catch (const std::exception &e)
  {
    std::cerr << "Exception: " << e.what() << std::endl;
    return -1;
  }

  //============================================================================================
  // 根据启动参数选择传输层
  // todo:多态的经典做法 基类指针→派生类对象（开头定义的基类指针transport，后续指向不同的派生类(SSE/HTTP/Stdio)
  // 1. `--sse`        -> 以 SSE 方式对外提供服务
  // 2. `--httpstream` -> 以 HTTP Stream 方式提供服务
  // 3. `默认无参数`    -> 使用 stdio，与宿主进程通过标准输入输出通信
  // [注]：
  // 这里完成的只是“通道选择”，即“创建对象+初始化参数”
  // 真正的 bind/listen/accept/IO读写，发生在后面调用的 Connect() 函数中。
  //============================================================================================
  if (use_sse_server->count() > 0)
  {
    transport = std::make_shared<vx::transport::SSE>();
  }
  else if (use_httpstream_server->count() > 0)
  {
    transport = std::make_shared<vx::transport::HttpStream>();
  }
  else
  {
    transport = std::make_shared<vx::transport::Stdio>();
  }

  //============================================================================================
  // 初始化日志系统
  // 日志文件名带时间戳，避免多次启动时相互覆盖，也方便按启动批次排查问题。
  //============================================================================================
  // 生成 ISO 8601 风格的时间戳字符串
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H-%M-%S");
  std::string iso_date = ss.str();

  // 将时间戳拼接到日志文件名中
  std::string logFilename = logs_directory + "/mcp-server_" + iso_date + ".log";
  auto sink_file =
      std::make_shared<AixLog::SinkFile>(AixLog::Severity::trace, logFilename);
  AixLog::Log::init({sink_file});

  //============================================================================================
  // 输出启动横幅和关键启动信息
  // 这里能直接看出版本号、选中的传输层类型，以及监听端口（如适用）。
  //============================================================================================
  LOG(INFO) << " __  __  _____ _____        _____ ______ _______      ________ "
               "_____  "
            << std::endl;
  LOG(INFO) << "|  \\/  |/ ____|  __ \\      / ____|  ____|  __ \\ \\    / /  "
               "____|  __ \\ "
            << std::endl;
  LOG(INFO) << "| \\  / | |    | |__) |____| (___ | |__  | |__) \\ \\  / /| "
               "|__  | |__) |"
            << std::endl;
  LOG(INFO) << "| |\\/| | |    |  ___/______\\___ \\|  __| |  _  / \\ \\/ / |  "
               "__| |  _  / "
            << std::endl;
  LOG(INFO) << "| |  | | |____| |          ____) | |____| | \\ \\  \\  /  | "
               "|____| | \\ \\ "
            << std::endl;
  LOG(INFO) << "|_|  |_|\\_____|_|         |_____/|______|_|  \\_\\  \\/   "
               "|______|_|  \\_\\"
            << std::endl;
  LOG(INFO) << "Starting mcp-server v" << PROJECT_VERSION
            << " (transport: " << transport->GetName() << " v"
            << transport->GetVersion() << ") on port: " << transport->GetPort()
            << std::endl;
  LOG(INFO) << "Press Ctrl+C to exit." << std::endl;

  // !==============================================================================
  // !【阶段二】插件加载和能力挂接：设置回调、加载插件、注册 MCP 请求处理器————最终启动服务器循环
  // !==============================================================================
  //============================================================================================
  // todo：插件loader-设置回调、加载插件
  // 1.注册插件回调：告诉插件管理器，以后插件发生某些事件时该做出什么反应（怎么通知外界）
  // 2.正式加载插件：注册好各种回调后，从指定目录加载插件，并输出加载结果日志。
  //============================================================================================

  // ?[注册回调]-SetOnPluginLoaded 插件加载成功回调
  // 在加载插件前设置回调，新插件加载成功后都会调用这个回调函数
  // 在回调函数中，我们为每个插件实例注入一个通知系统，使得插件能够主动向 MCP 客户端发送通知。
  loader->SetOnPluginLoaded(
      [](vx::mcp::PluginEntry &plugin)
      {
        plugin.instance->notifications = new NotificationSystem(); //
        plugin.instance->notifications->SendToClient = ClientNotificationCallbackImpl;
      });

  // ?[注册回调]-SetOnPluginsChanged 插件列表变化回调
  // 插件列表变化后按类型通知客户端重新拉取
  // todo:这里发送的是“列表已变化”的“通知”，而不是直接把完整列表推送给客户端。
  // todo:（MCP官方文档）客户端收到通知后，要主动发个“请求”来“响应”该通知
  // 也就是说，客户端要主动发起请求（调用method）：tools/list、prompts/list、resources/list 等来进行询问
  loader->SetOnPluginsChanged(
      [](bool toolsChanged, bool promptsChanged, bool resourcesChanged)
      {
        // 拉取最新快照。
        if (server && server->IsValid())
        {
          if (toolsChanged)
          {
            server->SendNotification(
                "mcp-server",
                MCPBuilder::NotificationToolsListChanged().dump().c_str());
          }
          if (promptsChanged)
          {
            server->SendNotification(
                "mcp-server",
                MCPBuilder::NotificationPromptsListChanged().dump().c_str());
          }
          if (resourcesChanged)
          {
            server->SendNotification(
                "mcp-server",
                MCPBuilder::NotificationResourcesListChanged().dump().c_str());
          }
        }
      });

  // ?[正式加载插件]-LoadPlugins
  // 注册完回调后，立即加载插件目录下的插件，并输出加载结果日志。
  if (loader->LoadPlugins(plugins_directory))
  {
    LOG(INFO) << "Successfully loaded plugins" << std::endl;
  }

  // ?[启动插件监听]-StartWatching
  // todo:插件“热更新机制”的核心————在后台启动一个线程，定期扫描插件目录的变化（新增、更新、删除），并根据变化动态加载/卸载插件。
  // !【插件“热更新”机制】：启动后台监听线程，每 5 秒扫描一次插件目录变化（新增、更新、删除）。
  // 这样服务启动后仍然支持插件热更新，而不需要重启整个进程。
  loader->StartWatching(plugins_directory, std::chrono::seconds(5));

  //============================================================================================
  // todo: 服务器Server-设置回调、注册 MCP-client 请求处理器
  // Server 内部本来有一套默认实现，在这里用插件驱动的实现覆盖掉，
  // 让 tools/prompts/resources 三类请求真正落到已加载插件上。
  //============================================================================================
  server->Name(name);
  server->VerboseLevel(verbose ? 1 : 0);

  // ?[注册 MCP 请求处理器]-OverrideCallback
  // todo:这部分的逻辑就像是nginx中的 “location配置”/路由，告诉服务器：当收到某个特定method的请求时，调用这个函数来处理。
  // todo:跟cgi协议很像
  // 1. tools/list 请求处理器：返回当前所有工具插件的列表和
  server->OverrideCallback("tools/list", [](const json &request)
                           {
    nlohmann::ordered_json response = MCPBuilder::Response(request);
    response["result"]["tools"] = json::array();

    // 先拿插件快照，再遍历快照中的工具插件；
    // 这样即使后台正在热加载，也不会长时间阻塞这里的请求处理。
    auto plugins = loader->GetPluginsSnapshot();
    for (const auto &plugin : plugins) {
      if (plugin->instance->GetType() == PLUGIN_TYPE_TOOLS) {
        for (int i = 0; i < plugin->instance->GetToolCount(); i++) {
          nlohmann::ordered_json tool;
          auto pluginTool = plugin->instance->GetTool(i);
          tool["name"] = pluginTool->name;
          tool["description"] = pluginTool->description;
          tool["inputSchema"] = nlohmann::json::parse(pluginTool->inputSchema);
          response["result"]["tools"].push_back(tool);
        }
      }
    }

    return response; });

  // 2. tools/call 请求处理器：根据工具名找到对应插件，并把原始请求转交给插件处理，最后把插件的响应再包装成 MCP 标准响应返回给客户端。
  server->OverrideCallback("tools/call", [](const json &request)
                           {
    nlohmann::ordered_json response = MCPBuilder::Response(request);

    // 按工具名查找目标插件，找到后把原始 MCP 请求直接转交给插件处理。
    // 插件返回的是 JSON 字符串，这里再解析回 JSON，拼装成标准 MCP 响应。
    auto plugins = loader->GetPluginsSnapshot();
    for (const auto &plugin : plugins) {
      if (plugin->instance->GetType() == PLUGIN_TYPE_TOOLS) {
        for (int i = 0; i < plugin->instance->GetToolCount(); i++) {
          auto pluginTool = plugin->instance->GetTool(i);
          if (pluginTool->name == request["params"]["name"]) {
            char *res_ptr =
                plugin->instance->HandleRequest(request.dump().c_str());
            if (res_ptr) {
              try {
                response["result"] = json::parse(res_ptr);
                response["result"]["isError"] = false;
              } catch (const json::parse_error &e) {
                // 插件返回了非合法 JSON 时，服务端兜底返回错误内容，
                // 避免客户端拿到半截响应或直接崩掉。
                response["result"]["isError"] = true;
                response["result"]["content"] = json::array();
                response["result"]["content"].push_back(
                    {{"type", "text"},
                     {"text", "Plugin returned malformed data."}});
              }
              delete[] res_ptr;
            } else {
              LOG(ERROR) << "Plugin " << pluginTool->name
                         << " returned nullptr." << std::endl;
            }
            return response;
          }
        }
      }
    }
    return response; });

  // 3. prompts/list 请求处理器：返回当前所有 Prompt 插件暴露的 prompt 元数据列表，供客户端浏览和选择。
  server->OverrideCallback("prompts/list", [](const json &request)
                           {
    nlohmann::ordered_json response = MCPBuilder::Response(request);
    response["result"]["prompts"] = json::array();

    // 汇总所有 Prompt 插件暴露出来的 prompt 元数据。
    auto plugins = loader->GetPluginsSnapshot();
    for (const auto &plugin : plugins) {
      if (plugin->instance->GetType() == PLUGIN_TYPE_PROMPTS) {
        for (int i = 0; i < plugin->instance->GetPromptCount(); i++) {
          nlohmann::ordered_json prompt;
          auto pluginPrompt = plugin->instance->GetPrompt(i);
          prompt["name"] = pluginPrompt->name;
          prompt["description"] = pluginPrompt->description;
          prompt["arguments"] = nlohmann::json::parse(pluginPrompt->arguments);
          response["result"]["prompts"].push_back(prompt);
        }
      }
    }

    return response; });

  // 4. prompts/get 请求处理器：根据 prompt 名称定位插件，并把完整请求交给插件生成 prompt 内容。
  server->OverrideCallback("prompts/get", [](const json &request)
                           {
    nlohmann::ordered_json response = MCPBuilder::Response(request);

    // 根据 prompt 名称定位插件，并把完整请求交给插件生成 prompt 内容。
    auto plugins = loader->GetPluginsSnapshot();
    for (const auto &plugin : plugins) {
      if (plugin->instance->GetType() == PLUGIN_TYPE_PROMPTS) {
        for (int i = 0; i < plugin->instance->GetPromptCount(); i++) {
          auto pluginPrompt = plugin->instance->GetPrompt(i);
          if (pluginPrompt->name == request["params"]["name"]) {
            char *res_ptr =
                plugin->instance->HandleRequest(request.dump().c_str());
            if (res_ptr) {
              try {
                response["result"] = json::parse(res_ptr);
              } catch (const json::parse_error &e) {
                LOG(ERROR) << "Plugin " << pluginPrompt->name
                           << " returned malformed data." << std::endl;
              }
              delete[] res_ptr;
            }
            return response;
          }
        }
      }
    }
    return response; });

  // 5. resources/list 请求处理器：返回当前所有资源插件暴露的资源清单，供客户端浏览和选择。
  server->OverrideCallback("resources/list", [](const json &request)
                           {
    nlohmann::ordered_json response = MCPBuilder::Response(request);
    response["result"]["resources"] = json::array();

    // 汇总所有资源插件暴露的资源清单，返回给客户端做浏览/选择。
    auto plugins = loader->GetPluginsSnapshot();
    for (const auto &plugin : plugins) {
      if (plugin->instance->GetType() == PLUGIN_TYPE_RESOURCES) {
        for (int i = 0; i < plugin->instance->GetResourceCount(); i++) {
          nlohmann::ordered_json resource;
          auto pluginResource = plugin->instance->GetResource(i);
          resource["name"] = pluginResource->name;
          resource["description"] = pluginResource->description;
          resource["uri"] = pluginResource->uri;
          resource["mimeType"] = pluginResource->mime;
          response["result"]["resources"].push_back(resource);
        }
      }
    }
    
    return response; });

  // 6. resources/read 请求处理器：根据资源 URI 找到具体资源提供者，再让插件返回实际资源内容。
  server->OverrideCallback("resources/read", [](const json &request)
                           {
    nlohmann::ordered_json response = MCPBuilder::Response(request);

    // 根据资源 URI 找到具体资源提供者，再让插件返回实际资源内容。
    auto plugins = loader->GetPluginsSnapshot();
    for (const auto &plugin : plugins) {
      if (plugin->instance->GetType() == PLUGIN_TYPE_RESOURCES) {
        for (int i = 0; i < plugin->instance->GetResourceCount(); i++) {
          auto pluginResource = plugin->instance->GetResource(i);
          if (pluginResource->uri == request["params"]["uri"]) {
            char *res_ptr =
                plugin->instance->HandleRequest(request.dump().c_str());
            if (res_ptr) {
              try {
                response["result"] = json::parse(res_ptr);
              } catch (const json::parse_error &e) {
                LOG(ERROR) << "Plugin " << pluginResource->name
                           << " returned malformed data." << std::endl;
              }
              delete[] res_ptr;
            }
          }
        }
      }
    }

    return response; });

  // ?[进入服务器主循环]-Connect
  // todo：在循环内部，Server会持续执行：“解析请求 -> 解析 JSON -> 按 method 分发 -> 写回响应”。
  server->Connect(transport);
  // 这是一个阻塞调用，直到以下任一情况发生才会返回：
  // 1. 客户端断开连接
  // 2. 收到 Ctrl+C，请求停止
  // 3. 传输层或解析过程出现不可继续的错误

  // ?[资源清理和优雅退出]
  // Connect 返回后，说明主服务循环已经结束（正常退出或停止中）。
  // 下面统一执行收尾逻辑，确保后台监控线程、插件对象、传输层都被有序释放。
  if (g_stopRequested)
  {
    LOG(INFO) << "Shutdown requested via signal, cleaning up..." << std::endl;
  }

  // 先停掉插件目录监听线程，再卸载插件，避免卸载过程中仍有热加载事件进来。
  loader->StopWatching();
  loader->UnloadPlugins();

  if (server && server->IsValid())
  {
    // 再次调用 Stop 属于兜底清理。
    // 如果 Connect 内部已经做过 Stop，Server 会用内部标志避免重复清理。
    server->Stop();
  }

  // 到这里，主流程完整结束：不再接收请求，也不会再向客户端发送通知。
  LOG(INFO) << "Server shutdown complete." << std::endl;

  return 0;
}
