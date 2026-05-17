//  The MIT License
//
//  Copyright (C) 2025 Giuseppe Mastrangelo
//
//  Permission is hereby granted, free of charge, to any person obtaining
//  a copy of this software and associated documentation files (the
//  'Software'), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be
//   included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#ifndef MCP_SERVER_PLUGINS_LOADER_H
#define MCP_SERVER_PLUGINS_LOADER_H

#ifdef _WIN32
#include <windows.h>
typedef HMODULE LibraryHandle;
#else
#include <dlfcn.h>
    typedef void* LibraryHandle;
#endif
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <atomic>
#include <shared_mutex>
#include <functional>
#include <chrono>
#include <set>
#include <map>

#include "aixlog.hpp"
#include "PluginAPI.h"

namespace vx::mcp {

    // 插件实例的完整生命周期由此结构体管理。
    // 当最后一个 shared_ptr 引用释放时，析构函数自动执行
    // Shutdown → delete notifications → DestroyPlugin → dlclose → 清理 staging 文件
    // 的完整清理流程。只要还有请求持有快照引用，插件就不会被卸载。
    struct PluginEntry {
        // 插件原始路径（用于对比目录变化）
        std::string path;
        // 实际加载的 staging 副本路径（dlopen 使用此路径，确保每次加载独立模块）
        std::string stagingPath;
        LibraryHandle handle = nullptr;
        PluginAPI* instance = nullptr;

        std::filesystem::file_time_type lastModified;
        std::uintmax_t fileSize = 0;

        PluginAPI* (*createFunc)() = nullptr;
        void (*destroyFunc)(PluginAPI*) = nullptr;

        PluginEntry() = default;
        PluginEntry(const PluginEntry&) = delete;
        PluginEntry& operator=(const PluginEntry&) = delete;

        ~PluginEntry() {
            if (instance) {
                instance->Shutdown();
                delete instance->notifications;
                instance->notifications = nullptr;
                destroyFunc(instance);
                instance = nullptr;
            }
            if (handle) {
#ifdef _WIN32
                FreeLibrary(handle);
#else
                dlclose(handle);
#endif
                handle = nullptr;
            }
            // 清理 staging 副本
            if (!stagingPath.empty() && stagingPath != path) {
                std::error_code ec;
                std::filesystem::remove(stagingPath, ec);
            }
        }
    };

    class PluginsLoader {
    public:
        using OnPluginLoaded = std::function<void(PluginEntry&)>;

        // 插件变更通知回调，参数标识哪些类型发生了变化
        using OnPluginsChanged = std::function<void(bool toolsChanged, bool promptsChanged, bool resourcesChanged)>;

        PluginsLoader();
        ~PluginsLoader();

        bool LoadPlugins(const std::string& directory);
        void UnloadPlugins();

        std::vector<std::shared_ptr<PluginEntry>> GetPluginsSnapshot() const;

        void SetOnPluginLoaded(OnPluginLoaded callback);
        void SetOnPluginsChanged(OnPluginsChanged callback);

        void StartWatching(const std::string& directory, std::chrono::seconds interval = std::chrono::seconds(5));
        void StopWatching();

    private:
        // CreatePluginInstance 的失败原因
        enum class LoadResult {
            kSuccess,              // 加载成功
            kLoadFailed,           // 插件本身加载/初始化失败，应记入失败缓存
            kSourceChangedDuringCopy  // 复制期间源文件被覆盖，不记入失败缓存，下轮重试
        };

        // 将插件文件复制到 staging 目录并从副本路径加载，确保独立模块
        std::shared_ptr<PluginEntry> CreatePluginInstance(const std::string& path, LoadResult& result);

        // 创建 staging 副本，返回副本路径。失败返回空字符串
        std::string CopyToStaging(const std::string& originalPath);

        // 确保 staging 目录存在
        void EnsureStagingDir(const std::string& pluginsDirectory);

        bool IsPluginFile(const std::string& extension) const;

        // 使用 std::filesystem::path 判断是否在 staging 目录下，跨平台兼容
        bool IsStagingPath(const std::filesystem::path& filePath) const;

        void WatchLoop(std::string directory, std::chrono::seconds interval);
        void ScanForChanges(const std::string& directory);

    private:
        std::vector<std::shared_ptr<PluginEntry>> m_plugins;
        mutable std::shared_mutex m_pluginsMutex;

        std::thread m_watchThread;
        std::atomic<bool> m_watching{false};

        OnPluginLoaded m_onPluginLoaded;
        OnPluginsChanged m_onPluginsChanged;

        std::string m_stagingDir;

        // 记录加载失败的插件的文件指纹（mtime+size），只有文件再次变化后才重试，避免持续重试坏插件
        struct FileFingerprint {
            std::filesystem::file_time_type mtime;
            std::uintmax_t size;
        };
        std::map<std::string, FileFingerprint> m_failedPlugins;
    };

}

#endif //MCP_SERVER_PLUGINS_LOADER_H
