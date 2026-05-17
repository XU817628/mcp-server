#include "PluginsLoader.h"
#include <map>
#include <sstream>

namespace vx::mcp {

    PluginsLoader::PluginsLoader() = default;

    PluginsLoader::~PluginsLoader() {
        StopWatching();
        UnloadPlugins();
        // 不在这里 remove_all(staging)：
        // UnloadPlugins 只是从列表移除 shared_ptr，外部快照可能仍持有引用，
        // PluginEntry 析构可能延迟执行。staging 文件由各 PluginEntry 析构时自行清理。
        // 残留的空 .staging 目录不影响功能，下次启动时会被复用。
    }

    void PluginsLoader::EnsureStagingDir(const std::string& pluginsDirectory) {
        if (!m_stagingDir.empty()) return;
        auto stagingPath = std::filesystem::path(pluginsDirectory) / ".staging";
        m_stagingDir = stagingPath.string();
        std::error_code ec;
        std::filesystem::create_directories(m_stagingDir, ec);
        if (ec) {
            LOG(ERROR) << "Failed to create staging directory: " << m_stagingDir
                       << " - " << ec.message() << std::endl;
        }
    }

    std::string PluginsLoader::CopyToStaging(const std::string& originalPath) {
        // 生成带时间戳的唯一文件名，确保 dlopen 加载独立模块
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        std::filesystem::path origPath(originalPath);
        std::string stem = origPath.stem().string();
        std::string ext = origPath.extension().string();

        std::ostringstream stagingName;
        stagingName << stem << "_" << ns << ext;

        auto stagingPath = std::filesystem::path(m_stagingDir) / stagingName.str();

        std::error_code ec;
        std::filesystem::copy_file(originalPath, stagingPath,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG(ERROR) << "Failed to copy plugin to staging: " << originalPath
                       << " -> " << stagingPath.string() << " - " << ec.message() << std::endl;
            return "";
        }

        return stagingPath.string();
    }

    // 将插件复制到 staging 路径后加载，确保即使原路径相同也能获得独立的动态库模块。
    // 加载成功后立即触发 OnPluginLoaded 回调完成初始化（如设置通知系统），
    // 保证插件在被加入 m_plugins 对请求线程可见之前已完全初始化。
    std::shared_ptr<PluginEntry> PluginsLoader::CreatePluginInstance(const std::string& path, LoadResult& result) {
        result = LoadResult::kLoadFailed;

        // 复制前先采集源文件指纹，确保指纹和 staging 副本内容一致
        std::filesystem::file_time_type preSnapshotMtime;
        std::uintmax_t preSnapshotSize;
        try {
            preSnapshotMtime = std::filesystem::last_write_time(path);
            preSnapshotSize = std::filesystem::file_size(path);
        } catch (const std::exception& ex) {
            LOG(ERROR) << "Cannot read plugin file metadata: " << path << " - " << ex.what() << std::endl;
            return nullptr;
        }

        // 复制到 staging 路径
        std::string stagingPath = CopyToStaging(path);
        if (stagingPath.empty()) {
            return nullptr;
        }

        // 复制后校验源文件是否在复制期间被覆盖
        try {
            auto postMtime = std::filesystem::last_write_time(path);
            auto postSize = std::filesystem::file_size(path);
            if (postMtime != preSnapshotMtime || postSize != preSnapshotSize) {
                LOG(WARNING) << "Plugin file changed during copy, will retry next scan: " << path << std::endl;
                std::error_code ec;
                std::filesystem::remove(stagingPath, ec);
                result = LoadResult::kSourceChangedDuringCopy;
                return nullptr;
            }
        } catch (const std::exception&) {
            std::error_code ec;
            std::filesystem::remove(stagingPath, ec);
            result = LoadResult::kSourceChangedDuringCopy;
            return nullptr;
        }

        auto entry = std::make_shared<PluginEntry>();
        entry->path = path;
        entry->stagingPath = stagingPath;
        entry->lastModified = preSnapshotMtime;
        entry->fileSize = preSnapshotSize;

#ifdef _WIN32
        entry->handle = LoadLibraryA(stagingPath.c_str());
        if (!entry->handle) {
            DWORD error = GetLastError();
            char errorMsg[256] = {0};
            FormatMessageA(
                    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    nullptr,
                    error,
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    errorMsg,
                    sizeof(errorMsg),
                    nullptr
            );
            LOG(ERROR) << "Failed to load plugin: " << path
                       << " - Error " << error << ": " << errorMsg << std::endl;
            entry->handle = nullptr;
            return nullptr;
        }
        entry->createFunc = (PluginAPI * (*)())GetProcAddress(entry->handle, "CreatePlugin");
        entry->destroyFunc = (void (*)(PluginAPI *))GetProcAddress(entry->handle, "DestroyPlugin");
#else
        entry->handle = dlopen(stagingPath.c_str(), RTLD_LAZY);
        if (!entry->handle) {
            LOG(ERROR) << "Failed to load plugin: " << path << " - " << dlerror() << std::endl;
            return nullptr;
        }

        entry->createFunc = (PluginAPI * (*)())dlsym(entry->handle, "CreatePlugin");
        entry->destroyFunc = (void (*)(PluginAPI *))dlsym(entry->handle, "DestroyPlugin");
#endif

        if (!entry->createFunc || !entry->destroyFunc) {
            LOG(ERROR) << "Plugin does not export required functions: " << path << std::endl;
            entry->createFunc = nullptr;
            entry->destroyFunc = nullptr;
            return nullptr;
        }

        entry->instance = entry->createFunc();
        if (!entry->instance) {
            LOG(ERROR) << "Plugin CreatePlugin() returned nullptr: " << path << std::endl;
            return nullptr;
        }

        if (!entry->instance->Initialize()) {
            LOG(ERROR) << "Plugin initialization failed: " << path << std::endl;
            entry->destroyFunc(entry->instance);
            entry->instance = nullptr;
            return nullptr;
        }

        LOG(INFO) << "Loaded plugin: " << entry->instance->GetName()
                  << " v" << entry->instance->GetVersion()
                  << " (from staging: " << stagingPath << ")" << std::endl;

        // 在加入列表前完成所有外部初始化，消除可见性窗口。
        // 回调失败视为激活失败，不发布该插件。
        if (m_onPluginLoaded) {
            try {
                m_onPluginLoaded(*entry);
            } catch (const std::exception& ex) {
                LOG(ERROR) << "OnPluginLoaded callback failed for " << path
                           << ": " << ex.what() << ", plugin will not be published" << std::endl;
                return nullptr;
            }
        }

        result = LoadResult::kSuccess;
        return entry;
    }

    bool PluginsLoader::LoadPlugins(const std::string& directory) {
        EnsureStagingDir(directory);

        try {
            std::vector<std::shared_ptr<PluginEntry>> newEntries;
            for (const auto& fsEntry : std::filesystem::recursive_directory_iterator(directory)) {
                if (fsEntry.is_regular_file() && IsPluginFile(fsEntry.path().extension().string())) {
                    if (IsStagingPath(fsEntry.path())) {
                        continue;
                    }
                    LoadResult result;
                    auto entry = CreatePluginInstance(fsEntry.path().string(), result);
                    if (entry) {
                        newEntries.push_back(std::move(entry));
                    } else if (result == LoadResult::kLoadFailed) {
                        try {
                            auto p = fsEntry.path().string();
                            m_failedPlugins[p] = {
                                std::filesystem::last_write_time(p),
                                std::filesystem::file_size(p)
                            };
                        } catch (...) {}
                    }
                }
            }

            {
                std::unique_lock lock(m_pluginsMutex);
                for (auto& entry : newEntries) {
                    m_plugins.push_back(std::move(entry));
                }
            }

            return true;
        } catch (const std::exception& ex) {
            LOG(ERROR) << "Error loading plugins: " << ex.what() << std::endl;
            return false;
        }
    }

    void PluginsLoader::UnloadPlugins() {
        std::vector<std::shared_ptr<PluginEntry>> pluginsToRelease;
        {
            std::unique_lock lock(m_pluginsMutex);
            pluginsToRelease = std::move(m_plugins);
            m_plugins.clear();
        }
        pluginsToRelease.clear();
    }

    std::vector<std::shared_ptr<PluginEntry>> PluginsLoader::GetPluginsSnapshot() const {
        std::shared_lock lock(m_pluginsMutex);
        return m_plugins;
    }

    void PluginsLoader::SetOnPluginLoaded(OnPluginLoaded callback) {
        m_onPluginLoaded = std::move(callback);
    }

    void PluginsLoader::SetOnPluginsChanged(OnPluginsChanged callback) {
        m_onPluginsChanged = std::move(callback);
    }

    bool PluginsLoader::IsPluginFile(const std::string& extension) const {
#ifdef _WIN32
        return extension == ".dll";
#elif __APPLE__
        return extension == ".dylib" || extension == ".so";
#else
        return extension == ".so";
#endif
    }

    // 通过规范化路径前缀比较判断文件是否位于 staging 目录下，跨平台兼容
    bool PluginsLoader::IsStagingPath(const std::filesystem::path& filePath) const {
        if (m_stagingDir.empty()) return false;
        // 使用 weakly_canonical 解析路径（不要求文件存在），然后做字符串前缀比较
        std::error_code ec;
        auto canonicalFile = std::filesystem::weakly_canonical(filePath, ec);
        if (ec) return false;
        auto canonicalStaging = std::filesystem::weakly_canonical(m_stagingDir, ec);
        if (ec) return false;
        auto fileStr = canonicalFile.string();
        auto stagingStr = canonicalStaging.string();
        return fileStr.size() > stagingStr.size()
            && fileStr.compare(0, stagingStr.size(), stagingStr) == 0
            && (fileStr[stagingStr.size()] == '/' || fileStr[stagingStr.size()] == '\\');
    }

    void PluginsLoader::StartWatching(const std::string& directory, std::chrono::seconds interval) {
        EnsureStagingDir(directory);
        if (m_watching.exchange(true)) return;
        m_watchThread = std::thread(&PluginsLoader::WatchLoop, this, directory, interval);
    }

    void PluginsLoader::StopWatching() {
        m_watching = false;
        if (m_watchThread.joinable()) {
            m_watchThread.join();
        }
    }

    void PluginsLoader::WatchLoop(std::string directory, std::chrono::seconds interval) {
        while (m_watching) {
            for (int i = 0; i < interval.count() && m_watching; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!m_watching) break;
            ScanForChanges(directory);
        }
    }

    void PluginsLoader::ScanForChanges(const std::string& directory) {
        try {
            struct FileInfo {
                std::filesystem::file_time_type mtime;
                std::uintmax_t size;
            };

            std::map<std::string, FileInfo> currentFiles;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file() && IsPluginFile(entry.path().extension().string())) {
                    if (IsStagingPath(entry.path())) {
                        continue;
                    }
                    FileInfo info;
                    info.mtime = entry.last_write_time();
                    info.size = entry.file_size();
                    currentFiles[entry.path().string()] = info;
                }
            }

            // 第一阶段（读锁）：只收集需要处理的路径，不设置 changed 标志
            std::vector<std::string> toUpdate;
            std::vector<std::string> toDelete;
            std::vector<std::string> toAdd;

            {
                std::shared_lock lock(m_pluginsMutex);
                for (const auto& plugin : m_plugins) {
                    auto fileIt = currentFiles.find(plugin->path);
                    if (fileIt == currentFiles.end()) {
                        toDelete.push_back(plugin->path);
                    } else if (fileIt->second.mtime != plugin->lastModified ||
                               fileIt->second.size != plugin->fileSize) {
                        // 跳过上次更新失败且文件未再变化的插件
                        auto failIt = m_failedPlugins.find(plugin->path);
                        if (failIt != m_failedPlugins.end()
                            && failIt->second.mtime == fileIt->second.mtime
                            && failIt->second.size == fileIt->second.size) {
                            currentFiles.erase(fileIt);
                        } else {
                            toUpdate.push_back(plugin->path);
                            currentFiles.erase(fileIt);
                        }
                    } else {
                        currentFiles.erase(fileIt);
                    }
                }
                for (const auto& [path, info] : currentFiles) {
                    // 跳过上次加载失败且文件未再变化的插件
                    auto failIt = m_failedPlugins.find(path);
                    if (failIt != m_failedPlugins.end()
                        && failIt->second.mtime == info.mtime
                        && failIt->second.size == info.size) {
                        continue;
                    }
                    toAdd.push_back(path);
                }
            }

            if (toUpdate.empty() && toAdd.empty() && toDelete.empty()) {
                return;
            }

            // 第二阶段（锁外）：创建新实例
            std::map<std::string, std::shared_ptr<PluginEntry>> updatedInstances;
            for (const auto& path : toUpdate) {
                LoadResult result;
                auto newInstance = CreatePluginInstance(path, result);
                if (newInstance) {
                    updatedInstances[path] = std::move(newInstance);
                    m_failedPlugins.erase(path);
                } else if (result == LoadResult::kLoadFailed) {
                    LOG(WARNING) << "Failed to reload plugin, keeping old version: " << path << std::endl;
                    try {
                        m_failedPlugins[path] = {
                            std::filesystem::last_write_time(path),
                            std::filesystem::file_size(path)
                        };
                    } catch (...) {}
                }
                // kSourceChangedDuringCopy: 不记入失败缓存，下轮自动重试
            }

            std::vector<std::shared_ptr<PluginEntry>> newInstances;
            for (const auto& path : toAdd) {
                LoadResult result;
                auto newInstance = CreatePluginInstance(path, result);
                if (newInstance) {
                    newInstances.push_back(std::move(newInstance));
                    m_failedPlugins.erase(path);
                } else if (result == LoadResult::kLoadFailed) {
                    try {
                        m_failedPlugins[path] = {
                            std::filesystem::last_write_time(path),
                            std::filesystem::file_size(path)
                        };
                    } catch (...) {}
                }
            }

            // 第三阶段（写锁）：执行实际变更，仅在此处确定 changed 和类型标记
            bool toolsChanged = false;
            bool promptsChanged = false;
            bool resourcesChanged = false;
            bool changed = false;

            auto markType = [&](PluginType type) {
                if (type == PLUGIN_TYPE_TOOLS) toolsChanged = true;
                else if (type == PLUGIN_TYPE_PROMPTS) promptsChanged = true;
                else if (type == PLUGIN_TYPE_RESOURCES) resourcesChanged = true;
            };

            std::vector<std::shared_ptr<PluginEntry>> oldEntries;
            {
                std::unique_lock lock(m_pluginsMutex);

                for (auto it = m_plugins.begin(); it != m_plugins.end(); ) {
                    auto updIt = updatedInstances.find((*it)->path);
                    if (updIt != updatedInstances.end()) {
                        // 实际执行替换：标记旧类型和新类型
                        LOG(INFO) << "Plugin updated, swapping: " << (*it)->path << std::endl;
                        if ((*it)->instance) markType((*it)->instance->GetType());
                        if (updIt->second->instance) markType(updIt->second->instance->GetType());
                        oldEntries.push_back(std::move(*it));
                        *it = std::move(updIt->second);
                        updatedInstances.erase(updIt);
                        changed = true;
                        ++it;
                    } else if (!std::filesystem::exists((*it)->path)) {
                        // 文件确实不存在才执行删除
                        LOG(INFO) << "Plugin file removed, unloading: " << (*it)->path << std::endl;
                        if ((*it)->instance) markType((*it)->instance->GetType());
                        m_failedPlugins.erase((*it)->path);
                        oldEntries.push_back(std::move(*it));
                        it = m_plugins.erase(it);
                        changed = true;
                    } else {
                        ++it;
                    }
                }

                for (auto& entry : newInstances) {
                    LOG(INFO) << "New plugin loaded: " << entry->path << std::endl;
                    if (entry->instance) markType(entry->instance->GetType());
                    m_plugins.push_back(std::move(entry));
                    changed = true;
                }
            }

            // 锁外释放旧插件
            oldEntries.clear();

            // 清理失败缓存中已不存在的文件记录
            for (auto it = m_failedPlugins.begin(); it != m_failedPlugins.end(); ) {
                if (!std::filesystem::exists(it->first)) {
                    it = m_failedPlugins.erase(it);
                } else {
                    ++it;
                }
            }

            if (changed && m_onPluginsChanged) {
                m_onPluginsChanged(toolsChanged, promptsChanged, resourcesChanged);
            }

        } catch (const std::exception& ex) {
            LOG(ERROR) << "Error scanning plugins: " << ex.what() << std::endl;
        }
    }

}
