#pragma once

#include "object.h"
#include "transaction.h"
#include "repository.h"
#include "resolver.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>

namespace okrapm {

// Extension API: 扩展框架
// 扩展可以注册: Object Type, Operation, Repository Backend, Artifact Backend, Resolver, Hook, Formatter
// 核心定义规则，扩展提供能力

// 扩展类型
enum class ExtensionType {
    ObjectType,        // 新对象类型
    Operation,         // 新操作
    RepositoryBackend, // 仓库后端
    ArtifactBackend,   // Artifact 后端
    Resolver,          // 依赖解析器
    Hook,              // 钩子 (事务前后回调)
    Formatter,         // 输出格式化器
    Plugin,            // 动态链接插件 (.so)
};

// 扩展信息
struct ExtensionInfo {
    std::string name;
    std::string version;
    std::string description;
    ExtensionType type;
    std::string file_path;
};

// Hook 类型 (事务生命周期回调)
enum class HookType {
    PreTransaction,   // 事务前
    PostTransaction,  // 事务后
    PreInstall,       // 安装前
    PostInstall,      // 安装后
    PreRemove,        // 删除前
    PostRemove,       // 删除后
};

// Forward declaration
class ExtensionApi;

// Hook 回调函数
using HookCallback = std::function<void(const Transaction&)>;

// Plugin Init / Cleanup 函数签名
using PluginInitFunc = bool (*)(ExtensionApi*);
using PluginCleanupFunc = void (*)();

// Extension API 注册表
class ExtensionApi {
public:
    static ExtensionApi& instance();

    // 注册扩展信息
    void register_extension(const ExtensionInfo& info);

    // 列出所有扩展
    std::vector<ExtensionInfo> list_extensions() const;

    // 注册 Hook
    void register_hook(HookType type, HookCallback callback);

    // 触发 Hooks
    void trigger_hooks(HookType type, const Transaction& txn) const;

    // 注册自定义操作
    using OperationHandler = std::function<bool(const std::vector<std::string>& args)>;
    void register_operation(const std::string& name, const std::string& description,
                           OperationHandler handler);

    // 执行自定义操作
    bool execute_operation(const std::string& name, const std::vector<std::string>& args) const;

    // 列出自定义操作
    std::vector<std::pair<std::string, std::string>> list_operations() const;

    // ---- 动态插件管理 (.so) ----
    bool load_plugin(const std::string& so_path);
    size_t load_plugins_from_directory(const std::string& dir_path);
    void unload_all();

private:
    ExtensionApi() = default;
    ~ExtensionApi();

    struct LoadedPlugin {
        void* handle{nullptr};
        bool oaabi{false};
        void (*fini)(){nullptr};
    };

    bool load_oaabi_plugin(const std::string& so_path);

    std::vector<ExtensionInfo> extensions_;
    std::unordered_map<HookType, std::vector<HookCallback>> hooks_;
    std::unordered_map<std::string, std::pair<std::string, OperationHandler>> operations_;
    std::vector<LoadedPlugin> plugin_handles_;
};

} // namespace okrapm
