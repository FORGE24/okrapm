#include "okrapmlib/lunar_core.h"
#include "okrapmlib/artifact_engine.h"
#include "opsis/interpreter.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cctype>

namespace fs = std::filesystem;

namespace {
std::string shell_sha256(const std::string& path) {
    std::string cmd = "sha256sum \"" + path + "\" 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    char buffer[128]{};
    std::string out;
    if (fgets(buffer, sizeof(buffer), pipe)) out = buffer;
    pclose(pipe);
    auto pos = out.find_first_of(" \\t");
    return pos == std::string::npos ? out : out.substr(0, pos);
}

bool verify_sidecar(const std::string& path) {
    std::ifstream sidecar(path + ".sha256");
    if (!sidecar) return true;
    std::string expected;
    sidecar >> expected;
    return !expected.empty() && expected == shell_sha256(path);
}

struct PayloadEdit {
    fs::path path;
    fs::path backup;
    bool directory{false};
};

// 包标识：首字符是字母或数字，其后可以是字母、数字、点、下划线、加号、减号。
bool package_id(const std::string& text) {
    if (text.empty() || !std::isalnum(static_cast<unsigned char>(text[0]))) return false;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '+' || ch == '-') continue;
        return false;
    }
    return true;
}

bool note_new_directories(const fs::path& dir, std::vector<PayloadEdit>& edits) {
    fs::path current;
    for (const fs::path& part : dir) {
        if (part.empty() || part == "/" || part == ".") {
            if (current.empty()) current = part;
            continue;
        }
        current /= part;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(current, ec))) continue;
        if (!fs::create_directory(current, ec) || ec) return false;
        PayloadEdit edit;
        edit.path = current;
        edit.directory = true;
        edits.push_back(std::move(edit));
    }
    return true;
}

bool place_node(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    auto status = fs::symlink_status(from, ec);
    if (ec) return false;
    if (fs::is_symlink(status)) {
        fs::create_symlink(fs::read_symlink(from, ec), to, ec);
        return !ec;
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::permissions(to, status.permissions(), ec);
    return true;
}

bool copy_payload(const fs::path& payload, const fs::path& root, std::vector<PayloadEdit>& edits,
    const fs::path& backup_root) {
    std::error_code ec;
    fs::create_directories(backup_root, ec);
    if (ec) return false;
    fs::recursive_directory_iterator it(
        payload, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        auto rel = fs::relative(entry.path(), payload, ec);
        if (ec || rel.empty() || rel.native().find("..") != std::string::npos) return false;
        auto dest = root / rel;
        if (entry.is_directory()) {
            if (!note_new_directories(dest, edits)) return false;
            continue;
        }
        if (!entry.is_symlink() && !entry.is_regular_file()) continue;
        if (!note_new_directories(dest.parent_path(), edits)) return false;
        fs::path backup;
        if (fs::exists(fs::symlink_status(dest, ec))) {
            if (fs::is_directory(fs::symlink_status(dest))) return false;
            backup = backup_root / std::to_string(edits.size());
            if (!place_node(dest, backup)) return false;
            fs::remove(dest, ec);
            if (ec) return false;
        }
        bool wrote = false;
        if (entry.is_symlink()) {
            auto target = fs::read_symlink(entry.path(), ec);
            if (!ec) fs::create_symlink(target, dest, ec);
            wrote = !ec;
        } else {
            wrote = place_node(entry.path(), dest);
        }
        PayloadEdit change;
        change.path = dest;
        change.backup = backup;
        edits.push_back(std::move(change));
        if (!wrote) return false;
    }
    return true;
}

void rollback_payload(std::vector<PayloadEdit>& edits) {
    std::error_code ec;
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        if (it->directory) {
            fs::remove(it->path, ec);
            continue;
        }
        fs::remove(it->path, ec);
        if (!it->backup.empty()) place_node(it->backup, it->path);
        fs::remove(it->backup, ec);
    }
    edits.clear();
}
}

namespace okrapm {

LunarCore::LunarCore(const std::string& data_dir)
    : data_dir_(data_dir) {
    std::error_code ec;
    fs::create_directories(data_dir_, ec);
    fs::create_directories(data_dir_ + "/repos", ec);
    fs::create_directories(data_dir_ + "/snapshots", ec);
    fs::create_directories(data_dir_ + "/extensions", ec);

    repo_mgr_ = std::make_unique<RepositoryManager>(data_dir_ + "/repos");
    system_store_ = std::make_unique<SystemStore>(data_dir_ + "/system.db");
    snapshot_mgr_ = std::make_unique<SnapshotManager>(data_dir_ + "/snapshots");

    resolver_.set_repository_manager(repo_mgr_.get());
    resolver_.set_installed_objects(system_store_->list_installed());

    // 注册内置扩展
    ExtensionInfo core_ext;
    core_ext.name = "lunar-core";
    core_ext.version = "1.0.0";
    core_ext.description = "Lunar native core engine";
    core_ext.type = ExtensionType::ObjectType;
    extensions().register_extension(core_ext);

    ExtensionInfo oaa_backend;
    oaa_backend.name = "lunar-oaa";
    oaa_backend.version = "1.0.0";
    oaa_backend.description = "Lunar Native Artifact (.oaa) Backend";
    oaa_backend.type = ExtensionType::ArtifactBackend;
    extensions().register_extension(oaa_backend);

    ExtensionInfo okra_backend;
    okra_backend.name = "okrapm";
    okra_backend.version = "3.0.0";
    okra_backend.description = "Okra Package Format (.okra) & Legacy okpm Compatibility Loader";
    okra_backend.type = ExtensionType::ArtifactBackend;
    extensions().register_extension(okra_backend);

    // 自动加载系统和用户插件
    extensions().load_plugins_from_directory(data_dir_ + "/extensions");
    extensions().load_plugins_from_directory("/usr/lib/lunar/extensions");
    extensions().load_plugins_from_directory("/var/lib/okpm/extensions");
}

LunarCore::InstallResult LunarCore::install(const std::vector<std::string>& refs, bool plan_only) {
    InstallResult res;
    if (refs.empty()) {
        res.error_message = "No objects specified to install";
        return res;
    }

    std::vector<ObjectRef> parsed_refs;
    std::vector<Object> artifact_objects;
    std::vector<std::string> artifact_paths;

    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (!parsed) {
            res.error_message = "Invalid object reference: " + r;
            return res;
        }

        if (parsed->is_artifact()) {
            // 本地 Artifact 文件安装
            auto meta = ArtifactExtractor::inspect(parsed->artifact_path());
            if (!meta) {
                res.error_message = "Failed to inspect artifact archive: " + parsed->artifact_path();
                return res;
            }
            auto obj = meta->to_object();
            // 将本地文件绝对路径保存至 repository 字段以便后续安装时解压
            obj.set_repository("local:" + fs::absolute(parsed->artifact_path()).string());
            artifact_objects.push_back(obj);
            artifact_paths.push_back(fs::absolute(parsed->artifact_path()).string());
        } else {
            parsed_refs.push_back(*parsed);
        }
    }

    resolver_.set_installed_objects(system_store_->list_installed());
    std::vector<Operation> all_ops;

    if (!parsed_refs.empty()) {
        auto resolve_res = resolver_.resolve_install(parsed_refs);
        if (!resolve_res.success) {
            res.error_message = resolve_res.errors.empty() ? "Dependency resolution failed" : resolve_res.errors[0];
            return res;
        }
        all_ops = resolve_res.operations;
    }

    for (size_t i = 0; i < artifact_objects.size(); ++i) {
        const auto& art_obj = artifact_objects[i];
        auto current = system_store_->find(art_obj.ns(), art_obj.name());
        if (current) {
            all_ops.emplace_back(OperationType::Update, art_obj, *current);
        } else {
            all_ops.emplace_back(OperationType::Install, art_obj);
        }
    }

    res.transaction = Transaction(all_ops);
    res.transaction.set_description("Install objects");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::RemoveResult LunarCore::remove(const std::vector<std::string>& refs, bool purge, bool plan_only) {
    RemoveResult res;
    if (refs.empty()) {
        res.error_message = "No objects specified to remove";
        return res;
    }

    std::vector<ObjectRef> parsed_refs;
    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (parsed) parsed_refs.push_back(*parsed);
    }

    resolver_.set_installed_objects(system_store_->list_installed());
    auto resolve_res = resolver_.resolve_remove(parsed_refs, purge);
    if (!resolve_res.success) {
        res.error_message = resolve_res.errors.empty() ? "Dependency resolution failed" : resolve_res.errors[0];
        return res;
    }

    res.transaction = Transaction(resolve_res.operations);
    res.transaction.set_description(purge ? "Purge objects" : "Remove objects");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::InstallResult LunarCore::sync(const std::vector<std::string>& targets) {
    InstallResult res;
    Transaction txn;
    txn.set_description("Sync repositories and objects");

    if (targets.empty()) {
        // 同步所有仓库
        for (const auto& repo : repo_mgr_->list()) {
            repo->sync();
        }
        res.success = true;
        return res;
    }

    for (const auto& target : targets) {
        auto repo = repo_mgr_->get(target);
        if (repo) {
            if (!repo->sync()) {
                res.success = false;
                res.error_message = "Failed to sync repository " + target;
                return res;
            }
        } else {
            // 同步指定系统对象或命名空间
            auto obj_opt = repo_mgr_->find(target);
            if (obj_opt) {
                Operation op(OperationType::Sync, *obj_opt);
                txn.add_operation(op);
            }
        }
    }

    if (!txn.operations().empty()) {
        res.transaction = txn;
        res.success = commit_transaction(res.transaction);
    } else {
        res.success = true;
    }
    return res;
}

LunarCore::InstallResult LunarCore::update(const std::vector<std::string>& refs, bool plan_only) {
    InstallResult res;
    std::vector<ObjectRef> parsed_refs;
    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (parsed) parsed_refs.push_back(*parsed);
    }

    resolver_.set_installed_objects(system_store_->list_installed());
    auto resolve_res = resolver_.resolve_update(parsed_refs);
    if (!resolve_res.success) {
        res.error_message = resolve_res.errors.empty() ? "Dependency resolution failed" : resolve_res.errors[0];
        return res;
    }

    res.transaction = Transaction(resolve_res.operations);
    res.transaction.set_description("Rolling system update");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::InstallResult LunarCore::upgradle(const std::vector<std::string>& targets, bool plan_only) {
    InstallResult res;
    std::vector<ObjectRef> parsed_refs;
    for (const auto& r : targets) {
        auto parsed = ObjectRef::parse(r);
        if (parsed) parsed_refs.push_back(*parsed);
    }

    auto resolve_res = resolver_.resolve_upgrade(parsed_refs);
    if (!resolve_res.success) {
        res.error_message = resolve_res.errors.empty() ? "Upgrade resolution failed" : resolve_res.errors[0];
        return res;
    }

    res.transaction = Transaction(resolve_res.operations);
    res.transaction.set_description("System-level baseline upgrade (Upgradle)");
    res.transaction.set_id(system_store_->next_state_id());
    res.transaction.advance_state(TransactionState::Planned);

    if (plan_only) {
        res.success = true;
        return res;
    }

    res.success = commit_transaction(res.transaction);
    if (!res.success) {
        res.error_message = "Transaction commit failed: " + res.transaction.error_message();
    }
    return res;
}

LunarCore::DownloadResult LunarCore::download(const std::vector<std::string>& refs, const std::string& dest_dir) {
    DownloadResult res;
    std::string target_dir = dest_dir.empty() ? (data_dir_ + "/cache/downloads") : dest_dir;
    std::error_code ec;
    fs::create_directories(target_dir, ec);

    for (const auto& r : refs) {
        auto parsed = ObjectRef::parse(r);
        if (!parsed) {
            res.error_message = "Invalid object ref: " + r;
            return res;
        }

        auto obj = repo_mgr_->find_by_ref(*parsed);
        if (!obj) {
            res.error_message = "Object not found in repositories: " + r;
            return res;
        }

        auto repo = repo_mgr_->get_repository_for_object(*obj);
        if (!repo) {
            res.error_message = "No repository found for object: " + r;
            return res;
        }

        auto path = repo->fetch_artifact(*obj, target_dir);
        if (path) {
            res.downloaded_paths.push_back(*path);
        } else {
            res.error_message = "Failed to download artifact for " + r;
            return res;
        }
    }

    res.success = true;
    return res;
}

Collection<Object> LunarCore::find(const std::string& pattern) const {
    auto all_objs = repo_mgr_->all_objects();
    return all_objs.where([&](const Object& obj) {
        return ObjectRef::matches_pattern(pattern, obj.full_name()) ||
               ObjectRef::matches_pattern(pattern, obj.ns()) ||
               ObjectRef::matches_pattern(pattern, obj.name());
    });
}

Collection<Object> LunarCore::search(const std::string& query) const {
    auto all_objs = repo_mgr_->all_objects();
    return all_objs.where([&](const Object& obj) {
        return obj.name().find(query) != std::string::npos ||
               obj.ns().find(query) != std::string::npos ||
               obj.description().find(query) != std::string::npos;
    });
}

Collection<Object> LunarCore::list_installed() const {
    return system_store_->collection();
}

std::optional<Object> LunarCore::info(const std::string& ref) const {
    auto parsed = ObjectRef::parse(ref);
    if (!parsed) return std::nullopt;

    auto inst = system_store_->find(parsed->ns(), parsed->name());
    if (inst) return inst;

    return repo_mgr_->find(parsed->ns(), parsed->name());
}

LunarCore::SystemStatus LunarCore::status() const {
    SystemStatus st;
    st.state_id = system_store_->next_state_id() - 1;
    st.installed_count = system_store_->list_installed().size();

    // 计算过期数
    size_t outdated = 0;
    for (const auto& inst : system_store_->list_installed()) {
        auto latest = repo_mgr_->find(inst.ns(), inst.name());
        if (latest && latest->version() > inst.version()) {
            ++outdated;
        }
    }
    st.outdated_count = outdated;

    for (const auto& repo : repo_mgr_->list()) {
        st.repositories.push_back(repo->name());
    }

    auto sys_ver = system_store_->find("okra", "systemversion");
    if (sys_ver) {
        st.system_version = sys_ver->version().to_string();
    } else {
        st.system_version = "Rolling State";
    }

    return st;
}

std::vector<Transaction> LunarCore::transaction_history() const {
    return transaction_history_;
}

std::optional<Transaction> LunarCore::get_transaction(uint64_t id) const {
    for (const auto& txn : transaction_history_) {
        if (txn.id() == id) return txn;
    }
    return std::nullopt;
}

Snapshot LunarCore::create_snapshot(const std::string& description) {
    return snapshot_mgr_->create(*system_store_, description);
}

bool LunarCore::rollback(uint64_t snapshot_id) {
    auto plan = snapshot_mgr_->restore_plan(snapshot_id, *system_store_);
    Transaction txn;
    txn.set_description("Rollback to Snapshot #" + std::to_string(snapshot_id));

    for (const auto& obj : plan.to_remove) {
        txn.add_operation(Operation(OperationType::Remove, obj));
    }
    for (const auto& obj : plan.to_install) {
        auto cur = system_store_->find(obj.ns(), obj.name());
        if (cur) {
            txn.add_operation(Operation(OperationType::Update, obj, *cur));
        } else {
            txn.add_operation(Operation(OperationType::Install, obj));
        }
    }

    bool success = commit_transaction(txn);
    if (success) {
        success = snapshot_mgr_->restore(snapshot_id, *system_store_);
    }
    return success;
}

bool LunarCore::commit_transaction(Transaction& txn) {
    struct AppliedPackage {
        std::vector<PayloadEdit> files;
        fs::path backup_root;
        fs::path program_live;
        fs::path program_previous;
        bool program_replaced{false};
    };
    std::vector<AppliedPackage> applied;
    std::vector<Operation> pending_store;
    txn.advance_state(TransactionState::Verified);
    extensions().trigger_hooks(HookType::PreTransaction, txn);

    txn.advance_state(TransactionState::Committing);

    // 事务前自动生成快照保护系统
    snapshot_mgr_->create(*system_store_, "Auto snapshot before txn " + std::to_string(txn.id()));

    auto rollback_applied = [](AppliedPackage& item) {
        rollback_payload(item.files);
        std::error_code ec;
        fs::remove_all(item.backup_root, ec);
        if (item.program_live.empty()) return;
        fs::remove_all(item.program_live, ec);
        if (item.program_replaced) fs::rename(item.program_previous, item.program_live, ec);
    };
    auto fail_transaction = [&](const std::string& message) {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) rollback_applied(*it);
        txn.advance_state(TransactionState::Failed, message);
        txn.advance_state(TransactionState::RolledBack);
        return false;
    };

    auto install_root_for = [&]() {
        const char* configured_root = std::getenv("LUNAR_INSTALL_ROOT");
        fs::path install_root = configured_root ? configured_root : "/";
        if (!configured_root && fs::status(install_root).permissions() != fs::perms::unknown &&
            (fs::status(install_root).permissions() & fs::perms::owner_write) == fs::perms::none) {
            install_root = data_dir_ + "/rootfs";
        }
        return install_root;
    };

    auto script_exists = [](const fs::path& staging, const std::string& stem) {
        return fs::is_regular_file(staging / "scripts" / (stem + ".opsis"))
            || fs::is_regular_file(staging / (stem + ".opsis"));
    };
    auto read_architecture = [](const fs::path& staging) {
        std::ifstream input(staging / "meta.yaml");
        std::string line;
        const std::string key = "architecture:";
        while (std::getline(input, line)) {
            if (line.compare(0, key.size(), key) != 0) continue;
            std::string value = line.substr(key.size());
            auto start = value.find_first_not_of(" \t");
            if (start == std::string::npos) break;
            value = value.substr(start);
            if (!value.empty() && value.front() == '"') value.erase(value.begin());
            if (!value.empty() && value.back() == '"') value.pop_back();
            if (!value.empty()) return value;
        }
        return std::string(Opsis::HostArchitecture());
    };
    auto scene_for = [&](const Operation& op, const fs::path& staging, const std::string& entry) {
        Opsis::PackageScene scene;
        scene.Namespace = op.target().ns();
        scene.Name = op.target().name();
        scene.Version = op.target().version().to_string();
        scene.NewVersion = scene.Version;
        if (auto current = system_store_->find(op.target().ns(), op.target().name())) {
            scene.OldVersion = current->version().to_string();
        }
        scene.Architecture = read_architecture(staging);
        scene.Reason = entry;
        return scene;
    };
    auto keep_program = [&](const fs::path& staging, const Object& target, AppliedPackage& item) {
        if (!package_id(target.ns()) || !package_id(target.name())) return false;
        fs::path live = fs::path(data_dir_) / "pkg-scripts" / target.ns() / target.name();
        fs::path incoming = live.parent_path() / (target.name() + ".incoming");
        fs::path previous = live.parent_path() / (target.name() + ".previous");
        std::error_code ec;
        fs::remove_all(incoming, ec);
        ec.clear();
        fs::create_directories(incoming, ec);
        if (ec) return false;
        const char* names[] = {
            "package.opsis", "lifecycle.opsis", "install.opsis", "update.opsis", "upgrade.opsis", "remove.opsis"
        };
        for (const char* name : names) {
            for (const fs::path& rel : {fs::path(name), fs::path("scripts") / name}) {
                fs::path src = staging / rel;
                if (!fs::is_regular_file(src)) continue;
                fs::path dest = incoming / rel;
                fs::create_directories(dest.parent_path(), ec);
                if (ec) {
                    fs::remove_all(incoming, ec);
                    return false;
                }
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    fs::remove_all(incoming, ec);
                    return false;
                }
            }
        }
        if (fs::is_directory(staging / "lib")) {
            fs::copy(staging / "lib", incoming / "lib", fs::copy_options::recursive, ec);
            if (ec) {
                fs::remove_all(incoming, ec);
                return false;
            }
        }
        bool replaced = false;
        if (fs::exists(live)) {
            fs::remove_all(previous, ec);
            ec.clear();
            fs::rename(live, previous, ec);
            if (ec) {
                fs::remove_all(incoming, ec);
                return false;
            }
            replaced = true;
        }
        fs::rename(incoming, live, ec);
        if (ec) {
            std::error_code undo;
            if (replaced) fs::rename(previous, live, undo);
            fs::remove_all(incoming, undo);
            return false;
        }
        item.program_live = live;
        item.program_previous = previous;
        item.program_replaced = replaced;
        return true;
    };

    // 安装先复制 payload。新安装调用 INSTALL，已安装对象调用 UPDATE。没有对应入口时走默认行为。
    auto apply_package = [&](const fs::path& staging, AppliedPackage& item, const Operation& op,
        bool upgrade) -> const char* {
        if (!package_id(op.target().ns()) || !package_id(op.target().name())) {
            return "invalid package identifier";
        }
        fs::path payload = fs::exists(staging / "files") ? staging / "files" : staging / "rootfs";
        bool has_payload = fs::is_directory(payload);
        bool has_opsis = script_exists(staging, "package") || script_exists(staging, "lifecycle")
            || script_exists(staging, "install") || script_exists(staging, "update")
            || script_exists(staging, "upgrade") || script_exists(staging, "remove");
        if (!has_payload && !has_opsis) return "package has no payload or install.opsis";
        if (has_payload && !copy_payload(payload, install_root_for(), item.files, item.backup_root)) {
            return "Artifact installation failed or file conflict detected";
        }
        std::string entry = upgrade ? "UPDATE" : "INSTALL";
        Opsis::PackageScene scene = scene_for(op, staging, entry);
        if (Opsis::RunLifecycleScript(staging.string(), entry, install_root_for().string(), false, scene) != 0) {
            return entry == "UPDATE" ? "OPSIS update script failed" : "OPSIS install script failed";
        }
        if (!keep_program(staging, op.target(), item)) return "failed to save package program";
        return nullptr;
    };

    for (const auto& op : txn.operations()) {
        switch (op.type()) {
            case OperationType::Install:
            case OperationType::Update:
            case OperationType::Upgrade:
            case OperationType::Sync: {
                if (!package_id(op.target().ns()) || !package_id(op.target().name())) {
                    return fail_transaction("invalid package identifier");
                }
                extensions().trigger_hooks(HookType::PreInstall, txn);
                if (op.target().repository().rfind("local:", 0) == 0) {
                    std::string local_path = op.target().repository().substr(6);
                    if (!fs::exists(local_path) || !verify_sidecar(local_path)) {
                        return fail_transaction("Local artifact SHA256 verification failed");
                    }
                    auto staging = fs::temp_directory_path() / ("lunar-install-" + std::to_string(txn.id()));
                    AppliedPackage item;
                    item.backup_root = fs::temp_directory_path()
                        / ("lunar-backup-" + std::to_string(txn.id()) + "-" + op.target().name());
                    bool extracted = ArtifactExtractor::extract(local_path, staging.string());
                    bool upgrade = op.type() == OperationType::Update || op.type() == OperationType::Upgrade
                        || system_store_->is_installed(op.target().ns(), op.target().name());
                    const char* package_error = extracted ? apply_package(staging, item, op, upgrade)
                                                          : "Local artifact installation failed";
                    if (package_error) {
                        rollback_applied(item);
                        fs::remove_all(staging);
                        return fail_transaction(package_error);
                    }
                    fs::remove_all(staging);
                    applied.push_back(std::move(item));
                } else if (op.target().repository() == "local-artifact") {
                    // 本地 artifact 的归档路径由 install() 传入并保留在目标对象中
                } else {
                    auto repo = repo_mgr_->get_repository_for_object(op.target());
                    // 远程仓库必须下载归档再复制。本地仓库里的对象可以只有元数据。
                    if (repo && repo->type_name() == "remote") {
                        auto art_path = repo->fetch_artifact(op.target());
                        if (!art_path || !fs::exists(*art_path)) {
                            return fail_transaction("Failed to fetch artifact for " + op.target().full_name());
                        }
                        auto staging = fs::temp_directory_path()
                            / ("lunar-install-" + std::to_string(txn.id()) + "-" + op.target().name());
                        AppliedPackage item;
                        item.backup_root = fs::temp_directory_path()
                            / ("lunar-backup-" + std::to_string(txn.id()) + "-" + op.target().name());
                        bool extracted = ArtifactExtractor::extract(*art_path, staging.string());
                        bool upgrade = op.type() == OperationType::Update || op.type() == OperationType::Upgrade
                            || system_store_->is_installed(op.target().ns(), op.target().name());
                        const char* package_error = extracted ? apply_package(staging, item, op, upgrade)
                                                              : "Artifact installation failed";
                        if (package_error) {
                            rollback_applied(item);
                            fs::remove_all(staging);
                            return fail_transaction(package_error);
                        }
                        fs::remove_all(staging);
                        applied.push_back(std::move(item));
                    }
                }
                pending_store.push_back(op);
                break;
            }
            case OperationType::Remove:
            case OperationType::Purge: {
                if (!package_id(op.target().ns()) || !package_id(op.target().name())) {
                    return fail_transaction("invalid package identifier");
                }
                fs::path saved = fs::path(data_dir_) / "pkg-scripts" / op.target().ns() / op.target().name();
                Opsis::PackageScene scene;
                scene.Namespace = op.target().ns();
                scene.Name = op.target().name();
                if (auto current = system_store_->find(op.target().ns(), op.target().name())) {
                    scene.Version = current->version().to_string();
                    scene.OldVersion = scene.Version;
                }
                scene.Reason = "REMOVE";
                if (Opsis::RunLifecycleScript(saved.string(), "REMOVE", install_root_for().string(),
                    false, scene) != 0) {
                    return fail_transaction("OPSIS remove script failed");
                }
                pending_store.push_back(op);
                break;
            }
        }
    }

    for (const auto& op : pending_store) {
        switch (op.type()) {
            case OperationType::Install:
            case OperationType::Update:
            case OperationType::Upgrade:
            case OperationType::Sync:
                system_store_->install(op.target());
                extensions().trigger_hooks(HookType::PostInstall, txn);
                break;
            case OperationType::Remove:
            case OperationType::Purge:
                extensions().trigger_hooks(HookType::PreRemove, txn);
                system_store_->remove(op.target().ns(), op.target().name());
                extensions().trigger_hooks(HookType::PostRemove, txn);
                break;
        }
    }

    if (!system_store_->save()) {
        system_store_->load();
        return fail_transaction("Failed to persist system store");
    }
    std::error_code ec;
    for (const auto& item : applied) {
        fs::remove_all(item.backup_root, ec);
        if (item.program_replaced) fs::remove_all(item.program_previous, ec);
    }
    for (const auto& op : pending_store) {
        if (op.type() != OperationType::Remove && op.type() != OperationType::Purge) continue;
        fs::remove_all(fs::path(data_dir_) / "pkg-scripts" / op.target().ns() / op.target().name(), ec);
    }

    system_store_->advance_state_id();
    txn.advance_state(TransactionState::Committed);

    extensions().trigger_hooks(HookType::PostTransaction, txn);
    record_transaction(txn);
    return true;
}

void LunarCore::record_transaction(const Transaction& txn) {
    transaction_history_.push_back(txn);
}

} // namespace okrapm
