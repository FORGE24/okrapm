#include "okrapmlib/extension_api.h"
#include "oaabi/oaabi.h"
#include <iostream>
#include <dlfcn.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace okrapm {

ExtensionApi& ExtensionApi::instance() {
    static ExtensionApi inst;
    return inst;
}

ExtensionApi::~ExtensionApi() {
    unload_all();
}

void ExtensionApi::register_extension(const ExtensionInfo& info) {
    // 避免重复注册
    for (auto& ext : extensions_) {
        if (ext.name == info.name) {
            ext = info;
            return;
        }
    }
    extensions_.push_back(info);
}

std::vector<ExtensionInfo> ExtensionApi::list_extensions() const {
    return extensions_;
}

void ExtensionApi::register_hook(HookType type, HookCallback callback) {
    hooks_[type].push_back(std::move(callback));
}

void ExtensionApi::trigger_hooks(HookType type, const Transaction& txn) const {
    auto it = hooks_.find(type);
    if (it != hooks_.end()) {
        for (const auto& cb : it->second) {
            try {
                cb(txn);
            } catch (const std::exception& e) {
                std::cerr << "ExtensionApi Hook error: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "ExtensionApi Hook unknown error\n";
            }
        }
    }
}

void ExtensionApi::register_operation(const std::string& name, const std::string& description,
                                      OperationHandler handler) {
    operations_[name] = {description, std::move(handler)};
    register_extension({name, "1.0.0", description, ExtensionType::Operation, ""});
}

bool ExtensionApi::execute_operation(const std::string& name, const std::vector<std::string>& args) const {
    auto it = operations_.find(name);
    if (it != operations_.end()) {
        return it->second.second(args);
    }
    std::cerr << "ExtensionApi: operation '" << name << "' not found.\n";
    return false;
}

std::vector<std::pair<std::string, std::string>> ExtensionApi::list_operations() const {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [name, pair] : operations_) {
        result.push_back({name, pair.first});
    }
    return result;
}

bool ExtensionApi::load_plugin(const std::string& so_path) {
    if (!fs::exists(so_path)) {
        return false;
    }

    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        std::cerr << "Failed to load plugin " << so_path << ": " << dlerror() << "\n";
        return false;
    }

    // 查找入口函数
    PluginInitFunc init_fn = reinterpret_cast<PluginInitFunc>(dlsym(handle, "lunar_plugin_init"));
    if (!init_fn) {
        dlclose(handle);
        return load_oaabi_plugin(so_path);
    }

    if (!init_fn(this)) {
        std::cerr << "Plugin initialization failed: " << so_path << "\n";
        dlclose(handle);
        return false;
    }

    plugin_handles_.push_back({handle, false, nullptr});

    std::string stem = fs::path(so_path).stem().string();
    if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
    register_extension({stem, "1.0.0", "Dynamic shared plugin", ExtensionType::Plugin, so_path});

    return true;
}

bool ExtensionApi::load_oaabi_plugin(const std::string& so_path) {
    // 旧入口保持 RTLD_GLOBAL。OAABI 入口按规范用立即绑定和局部符号。
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::cerr << "Failed to load OAABI plugin " << so_path << ": " << dlerror() << "\n";
        return false;
    }

    auto* plugin = reinterpret_cast<const OaabiPlugin*>(dlsym(handle, "OaabiPlugin"));
    if (!plugin) {
        std::cerr << "Plugin " << so_path << " missing 'lunar_plugin_init' or 'OaabiPlugin'\n";
        dlclose(handle);
        return false;
    }
    if (plugin->Version != OaabiVersion || plugin->Reserved != 0 || !plugin->Init) {
        std::cerr << "Plugin " << so_path << " has an unsupported OaabiPlugin entry\n";
        dlclose(handle);
        return false;
    }
    if (plugin->Init() != 0) {
        std::cerr << "OAABI plugin initialization failed: " << so_path << "\n";
        dlclose(handle);
        return false;
    }

    plugin_handles_.push_back({handle, true, plugin->Fini});
    std::string stem = fs::path(so_path).stem().string();
    if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
    register_extension({stem, "1.0.0", "OAABI plugin", ExtensionType::Plugin, so_path});
    return true;
}

size_t ExtensionApi::load_plugins_from_directory(const std::string& dir_path) {
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".so") {
            if (load_plugin(entry.path().string())) {
                count++;
            }
        }
    }
    return count;
}

void ExtensionApi::unload_all() {
    for (const LoadedPlugin& plugin : plugin_handles_) {
        if (!plugin.handle) continue;
        if (plugin.oaabi) {
            if (plugin.fini) plugin.fini();
        } else {
            PluginCleanupFunc cleanup_fn = reinterpret_cast<PluginCleanupFunc>(
                dlsym(plugin.handle, "lunar_plugin_cleanup"));
            if (cleanup_fn) cleanup_fn();
        }
        dlclose(plugin.handle);
    }
    plugin_handles_.clear();
}

} // namespace okrapm
