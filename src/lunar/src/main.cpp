#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <fstream>
#include <ctime>
#include "okrapmlib/lunar_core.h"
#include "okrapmlib/artifact_engine.h"
#include "okrapmlib/pipeline_engine.h"
#include "cli_text.h"

using namespace okrapm;

static std::string JoinWords(const std::vector<std::string> &Words)
{
	std::string Text;
	for (size_t Index = 0; Index < Words.size(); ++Index) {
		if (Index) Text += " ";
		Text += Words[Index];
	}
	return Text;
}

void print_help() {
    Cli::Help();
}

static std::vector<std::string> expand_stdin_targets(const std::vector<std::string>& targets) {
    std::vector<std::string> result;
    for (const auto& t : targets) {
        if (t == "-") {
            std::string line;
            while (std::getline(std::cin, line)) {
                auto first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) continue;
                auto last = line.find_last_not_of(" \t\r\n");
                std::string item = line.substr(first, last - first + 1);
                if (!item.empty()) result.push_back(item);
            }
        } else {
            result.push_back(t);
        }
    }
    return result;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string data_dir = "/var/lib/lunar";
    const char* env_data_dir = std::getenv("LUNAR_DATA_DIR");
    if (env_data_dir) data_dir = env_data_dir;

    std::vector<std::string> raw_args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--root" || arg == "-r") && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg.rfind("--root=", 0) == 0) {
            data_dir = arg.substr(7);
        } else {
            raw_args.push_back(arg);
        }
    }

    if (raw_args.empty()) {
        print_help();
        return 1;
    }

    std::string command = raw_args[0];

    if (command == "-h" || command == "--help" || command == "help") {
        print_help();
        return 0;
    }

    LunarCore core(data_dir);

    // ---- Pipeline Stream Processing (pipe) ----
    if (command == "pipe") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar pipe '<expr>'");
            return 1;
        }
        std::ostringstream oss;
        for (size_t i = 1; i < raw_args.size(); ++i) {
            if (i > 1) oss << " ";
            oss << raw_args[i];
        }
        std::string pipe_expr = oss.str();
        auto pipe_res = PipelineEngine::execute(pipe_expr, core);
        if (!pipe_res.success && !pipe_res.error_message.empty()) {
            Cli::TaskFail(pipe_res.error_message);
            return 1;
        }
        std::cout << pipe_res.output;
        return 0;
    }

    // ---- Artifact Packaging (build) ----
    if (command == "build") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar build <source_dir> [output_file.oaa]");
            return 1;
        }
        std::string src_dir = raw_args[1];
        ArtifactBuilder::BuildOptions opts;
        if (raw_args.size() >= 3) {
            opts.output_path = raw_args[2];
        }
        Cli::TaskBegin("build", src_dir);
        auto built = ArtifactBuilder::build(src_dir, opts);
        if (built) {
            Cli::TaskOk(*built);
            return 0;
        } else {
            Cli::TaskFail("failed to build artifact");
            return 1;
        }
    }

    // ---- Artifact Verification (verify) ----
    if (command == "verify") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar verify <file.oaa>");
            return 1;
        }
        const std::string& file = raw_args[1];
        auto meta = ArtifactExtractor::inspect(file);
        if (!meta) {
            Cli::TaskFail("cannot read " + file);
            return 1;
        }
        std::ifstream sidecar(file + ".sha256");
        if (sidecar) {
            std::string expected;
            sidecar >> expected;
            if (expected.empty() || expected != ArtifactExtractor::calculate_sha256(file)) {
                Cli::TaskFail("sha256 mismatch");
                return 1;
            }
        }
        Cli::TaskBegin("verify", file);
        Cli::TaskOk(ArtifactExtractor::calculate_sha256(file));
        return 0;
    }

    // ---- Artifact Sub-commands (artifact) ----
    if (command == "artifact") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar artifact <inspect|verify|extract> ...");
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "verify" && raw_args.size() >= 3) {
            std::string file = raw_args[2];
            auto meta = ArtifactExtractor::inspect(file);
            if (!meta) {
                Cli::TaskFail("cannot read " + file);
                return 1;
            }
            std::ifstream sidecar(file + ".sha256");
            if (sidecar) {
                std::string expected;
                sidecar >> expected;
                if (expected.empty() || expected != ArtifactExtractor::calculate_sha256(file)) {
                    Cli::TaskFail("sha256 mismatch");
                    return 1;
                }
            }
            if (!meta->checksum.empty() && meta->checksum != ArtifactExtractor::calculate_sha256(file)) {
                Cli::TaskFail("embedded sha256 mismatch");
                return 1;
            }
            Cli::TaskBegin("verify", file);
            Cli::TaskOk(ArtifactExtractor::calculate_sha256(file));
            return 0;
        } else if (sub == "inspect" && raw_args.size() >= 3) {
            std::string file = raw_args[2];
            auto meta = ArtifactExtractor::inspect(file);
            if (!meta) {
                Cli::TaskFail("cannot read " + file);
                return 1;
            }
            std::string deps;
            for (size_t i = 0; i < meta->dependencies.size(); ++i) {
                if (i) deps += " ";
                deps += meta->dependencies[i];
            }
            std::vector<std::pair<std::string, std::string>> fields = {
                {"path", file},
                {"namespace", meta->ns},
                {"name", meta->name},
                {"version", meta->version.to_string()},
                {"architecture", meta->architecture},
                {"description", meta->description},
                {"maintainer", meta->maintainer},
                {"sha256", meta->checksum},
                {"size", std::to_string(meta->download_size / 1024) + " KB"}
            };
            if (!deps.empty()) fields.push_back({"dependencies", deps});
            Cli::PrintFields(fields);
            return 0;
        } else if (sub == "extract" && raw_args.size() >= 4) {
            std::string file = raw_args[2];
            std::string dest = raw_args[3];
            Cli::TaskBegin("extract", file);
            if (ArtifactExtractor::extract(file, dest, /*verbose=*/false)) {
                Cli::TaskOk(dest);
                return 0;
            } else {
                Cli::TaskFail("extraction failed");
                return 1;
            }
        }
    }

    // ---- Plan 预览模式 ----
    if (command == "plan") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar plan <install|remove|update|upgradle> <ref...>");
            return 1;
        }
        std::string sub_cmd = raw_args[1];
        std::vector<std::string> args(raw_args.begin() + 2, raw_args.end());
        args = expand_stdin_targets(args);

        if (sub_cmd == "install") {
            auto res = core.install(args, /*plan_only=*/true);
            Cli::TaskBegin("plan install", JoinWords(args));
            if (!res.success && res.transaction.operations().empty()) {
                Cli::TaskFail(res.error_message);
                return 1;
            }
            Cli::PrintOperations(res.transaction);
            Cli::TaskOk(res.transaction.operations().empty() ? "nothing to do" : "not applied");
            return 0;
        } else if (sub_cmd == "remove") {
            auto res = core.remove(args, /*purge=*/false, /*plan_only=*/true);
            Cli::TaskBegin("plan remove", JoinWords(args));
            if (!res.success && res.transaction.operations().empty()) {
                Cli::TaskFail(res.error_message);
                return 1;
            }
            Cli::PrintOperations(res.transaction);
            Cli::TaskOk(res.transaction.operations().empty() ? "nothing to do" : "not applied");
            return 0;
        } else if (sub_cmd == "update") {
            auto res = core.update(args, /*plan_only=*/true);
            Cli::TaskBegin("plan update", JoinWords(args));
            if (!res.success && res.transaction.operations().empty()) {
                Cli::TaskFail(res.error_message);
                return 1;
            }
            Cli::PrintOperations(res.transaction);
            Cli::TaskOk(res.transaction.operations().empty() ? "nothing to do" : "not applied");
            return 0;
        } else if (sub_cmd == "upgradle") {
            auto res = core.upgradle(args, /*plan_only=*/true);
            Cli::TaskBegin("plan upgradle", JoinWords(args));
            if (!res.success && res.transaction.operations().empty()) {
                Cli::TaskFail(res.error_message);
                return 1;
            }
            Cli::PrintOperations(res.transaction);
            Cli::TaskOk(res.transaction.operations().empty() ? "nothing to do" : "not applied");
            return 0;
        } else {
            Cli::Error("unknown plan command: " + sub_cmd);
            return 1;
        }
    }

    // ---- Install ----
    if (command == "install") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar install <ref...>");
            return 1;
        }
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        Cli::TaskBegin("install", JoinWords(targets));
        auto res = core.install(targets);
        if (res.success) {
            Cli::PrintOperations(res.transaction);
            size_t count = res.transaction.operations().size();
            Cli::TaskOk(std::to_string(count) + (count == 1 ? " package" : " packages"));
            return 0;
        }
        Cli::TaskFail(res.error_message);
        return 1;
    }

    // ---- Install Group ----
    if (command == "install-group") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar install-group <name>");
            return 1;
        }
        std::string group_ref = raw_args[1];
        if (group_ref.empty() || group_ref[0] != '#') {
            group_ref = "#" + group_ref;
        }
        Cli::TaskBegin("install", group_ref);
        auto res = core.install({group_ref});
        if (res.success) {
            Cli::PrintOperations(res.transaction);
            size_t count = res.transaction.operations().size();
            Cli::TaskOk(std::to_string(count) + (count == 1 ? " package" : " packages"));
            return 0;
        }
        Cli::TaskFail(res.error_message);
        return 1;
    }

    // ---- Download (Fetch only) ----
    if (command == "download") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar download <ref...>");
            return 1;
        }
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        Cli::TaskBegin("download", JoinWords(targets));
        auto res = core.download(targets);
        if (res.success) {
            std::vector<std::vector<std::string>> rows;
            for (const auto& path : res.downloaded_paths) rows.push_back({path});
            Cli::PrintTable({"PATH"}, rows, 2);
            Cli::TaskOk(std::to_string(res.downloaded_paths.size()) + " files");
            return 0;
        }
        Cli::TaskFail(res.error_message);
        return 1;
    }

    // ---- Remove / Purge ----
    if (command == "remove" || command == "purge") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar " + command + " <ref...>");
            return 1;
        }
        bool is_purge = (command == "purge");
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        Cli::TaskBegin(command, JoinWords(targets));
        auto res = core.remove(targets, is_purge);
        if (res.success) {
            Cli::PrintOperations(res.transaction);
            size_t count = res.transaction.operations().size();
            Cli::TaskOk(std::to_string(count) + (count == 1 ? " package" : " packages"));
            return 0;
        }
        Cli::TaskFail(res.error_message);
        return 1;
    }

    // ---- Update (Rolling update) ----
    if (command == "update") {
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        Cli::TaskBegin("update", JoinWords(targets));
        auto res = core.update(targets);
        if (!res.success) {
            Cli::TaskFail(res.error_message);
            return 1;
        }
        if (res.transaction.operations().empty()) {
            Cli::TaskOk("already current");
            return 0;
        }
        Cli::PrintOperations(res.transaction);
        size_t count = res.transaction.operations().size();
        Cli::TaskOk(std::to_string(count) + (count == 1 ? " package" : " packages"));
        return 0;
    }

    // ---- Upgradle (System-level baseline upgrade) ----
    if (command == "upgradle") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar upgradle <target...>");
            return 1;
        }
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());

        Cli::TaskBegin("upgradle", JoinWords(targets));
        auto res = core.upgradle(targets);
        if (res.success) {
            Cli::PrintOperations(res.transaction);
            Cli::TaskOk("baseline shifted");
            return 0;
        }
        Cli::TaskFail(res.error_message);
        return 1;
    }

    // ---- Sync ----
    if (command == "sync") {
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());

        Cli::TaskBegin("sync", JoinWords(targets));
        auto res = core.sync(targets);
        if (res.success) {
            Cli::TaskOk(targets.empty() ? "all repositories" : JoinWords(targets));
            return 0;
        }
        Cli::TaskFail(res.error_message.empty() ? "sync failed" : res.error_message);
        return 1;
    }

    // ---- Find (Pattern query) ----
    if (command == "find") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar find <pattern>");
            return 1;
        }
        std::string pattern = raw_args[1];
        auto col = core.find(pattern);
        std::vector<std::vector<std::string>> rows;
        for (const auto& obj : col.to_vector()) {
            rows.push_back({obj.ref_string(), obj.version().to_string(), obj.description()});
        }
        Cli::PrintTable({"OBJECT", "VERSION", "DESCRIPTION"}, rows);
        return 0;
    }

    // ---- Search ----
    if (command == "search") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar search <query>");
            return 1;
        }
        std::string query = raw_args[1];
        auto col = core.search(query);
        std::vector<std::vector<std::string>> rows;
        for (const auto& obj : col.to_vector()) {
            rows.push_back({obj.ref_string(), obj.version().to_string(), obj.description()});
        }
        Cli::PrintTable({"OBJECT", "VERSION", "DESCRIPTION"}, rows);
        return 0;
    }

    // ---- List Installed ----
    if (command == "list") {
        auto col = core.list_installed();
        std::vector<std::vector<std::string>> rows;
        for (const auto& obj : col.to_vector()) {
            rows.push_back({obj.ns(), obj.name(), obj.version().to_string(), obj.repository()});
        }
        Cli::PrintTable({"NAMESPACE", "NAME", "VERSION", "REPOSITORY"}, rows);
        return 0;
    }

    // ---- Info ----
    if (command == "info") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar info <ref>");
            return 1;
        }
        auto obj = core.info(raw_args[1]);
        if (!obj) {
            Cli::Error("object not found: " + raw_args[1]);
            return 1;
        }
        std::string deps;
        for (size_t i = 0; i < obj->dependencies().size(); ++i) {
            if (i) deps += " ";
            deps += obj->dependencies()[i];
        }
        std::vector<std::pair<std::string, std::string>> fields = {
            {"object", obj->ref_string()},
            {"namespace", obj->ns()},
            {"name", obj->name()},
            {"version", obj->version().to_string()},
            {"type", Object::type_name(obj->type())},
            {"repository", obj->repository()},
            {"description", obj->description()},
            {"download", std::to_string(obj->download_size() / 1024) + " KB"},
            {"install", std::to_string(obj->installed_size() / 1024) + " KB"}
        };
        if (!deps.empty()) fields.push_back({"dependencies", deps});
        Cli::PrintFields(fields);
        return 0;
    }

    // ---- Members (Group expand) ----
    if (command == "members") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar members <#group>");
            return 1;
        }
        std::string group_ref = raw_args[1];
        if (!group_ref.empty() && group_ref[0] == '#') group_ref = group_ref.substr(1);
        auto obj = core.info(group_ref);
        if (!obj) {
            Cli::Error("group not found: " + raw_args[1]);
            return 1;
        }
        std::vector<std::vector<std::string>> rows;
        for (const auto& mem : obj->dependencies()) rows.push_back({mem});
        Cli::PrintTable({"MEMBER"}, rows);
        return 0;
    }

    // ---- Status ----
    if (command == "status") {
        auto st = core.status();
        std::string repos;
        for (size_t i = 0; i < st.repositories.size(); ++i) {
            if (i) repos += " ";
            repos += st.repositories[i];
        }
        Cli::PrintFields({
            {"state", "#" + std::to_string(st.state_id)},
            {"baseline", st.system_version},
            {"installed", std::to_string(st.installed_count)},
            {"outdated", std::to_string(st.outdated_count)},
            {"repositories", repos}
        });
        return 0;
    }

    // ---- Transaction ----
    if (command == "transaction") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar transaction <list|show <id>>");
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            auto history = core.transaction_history();
            std::vector<std::vector<std::string>> rows;
            for (const auto& txn : history) {
                rows.push_back({
                    std::to_string(txn.id()),
                    Transaction::state_name(txn.state()),
                    std::to_string(txn.operations().size()),
                    txn.description()
                });
            }
            Cli::PrintTable({"ID", "STATE", "OPS", "DESCRIPTION"}, rows);
            return 0;
        } else if (sub == "show" && raw_args.size() >= 3) {
            uint64_t id = std::stoull(raw_args[2]);
            auto txn = core.get_transaction(id);
            if (!txn) {
                Cli::Error("transaction not found: " + std::to_string(id));
                return 1;
            }
            Cli::TaskBegin("transaction", "#" + std::to_string(id));
            Cli::PrintOperations(*txn);
            Cli::TaskOk(Transaction::state_name(txn->state()));
            return 0;
        }
    }

    // ---- Snapshot & Rollback ----
    if (command == "snapshot") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar snapshot <list|create [desc]>");
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            auto snaps = core.snapshots().list();
            std::vector<std::vector<std::string>> rows;
            for (const auto& snap : snaps) {
                std::time_t stamp = std::chrono::system_clock::to_time_t(snap.timestamp);
                std::tm broken{};
                localtime_r(&stamp, &broken);
                std::ostringstream clock;
                clock << std::put_time(&broken, "%Y-%m-%d %H:%M:%S");
                rows.push_back({
                    std::to_string(snap.id),
                    clock.str(),
                    std::to_string(snap.objects.size()),
                    snap.description
                });
            }
            Cli::PrintTable({"ID", "TIME", "OBJECTS", "DESCRIPTION"}, rows);
            return 0;
        } else if (sub == "create") {
            std::string desc = (raw_args.size() >= 3) ? raw_args[2] : "Manual Snapshot";
            Cli::TaskBegin("snapshot", desc);
            auto snap = core.create_snapshot(desc);
            Cli::TaskOk("#" + std::to_string(snap.id));
            return 0;
        }
    }

    if (command == "rollback") {
        if (raw_args.size() < 2) {
            auto snaps = core.snapshots().list();
            if (snaps.empty()) {
                Cli::Error("no snapshots");
                return 1;
            }
            uint64_t latest_id = snaps.back().id;
            Cli::TaskBegin("rollback", "#" + std::to_string(latest_id));
            if (core.rollback(latest_id)) {
                Cli::TaskOk("#" + std::to_string(latest_id));
                return 0;
            }
            Cli::TaskFail("rollback failed");
            return 1;
        } else {
            std::string snap_str = raw_args[1];
            if (!snap_str.empty() && snap_str[0] == '#') snap_str = snap_str.substr(1);
            uint64_t snap_id = std::stoull(snap_str);
            Cli::TaskBegin("rollback", "#" + std::to_string(snap_id));
            if (core.rollback(snap_id)) {
                Cli::TaskOk("#" + std::to_string(snap_id));
                return 0;
            }
            Cli::TaskFail("rollback failed");
            return 1;
        }
    }

    // ---- Repository ----
    if (command == "repo") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar repo <list|add|remove|enable|disable>");
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            std::vector<std::vector<std::string>> rows;
            for (const auto& repo : core.repositories().list()) {
                rows.push_back({
                    repo->name(),
                    repo->type_name(),
                    repo->enabled() ? "enabled" : "disabled",
                    repo->url()
                });
            }
            Cli::PrintTable({"NAME", "TYPE", "STATE", "URL"}, rows);
            return 0;
        } else if (sub == "add" && raw_args.size() >= 4) {
            std::string name = raw_args[2];
            std::string url = raw_args[3];
            std::string type = (raw_args.size() >= 5) ? raw_args[4] : "";
            if (type.empty()) {
                if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 ||
                    url.rfind("file://", 0) == 0) {
                    type = "remote";
                } else {
                    type = "local";
                }
            }

            if (type == "remote") {
                core.repositories().add(std::make_shared<RemoteRepository>(name, url, data_dir + "/repos/" + name));
            } else {
                core.repositories().add(std::make_shared<LocalRepository>(name, data_dir + "/repos/" + name + "/repo.db", url));
            }
            Cli::TaskBegin("repo add", name);
            Cli::TaskOk(url);
            return 0;
        } else if (sub == "add" && raw_args.size() == 3) {
            std::string name = raw_args[2];
            core.repositories().add(std::make_shared<LocalRepository>(name, data_dir + "/repos/" + name + "/repo.db"));
            Cli::TaskBegin("repo add", name);
            Cli::TaskOk("local");
            return 0;
        } else if (sub == "remove" && raw_args.size() >= 3) {
            core.repositories().remove(raw_args[2]);
            Cli::TaskBegin("repo remove", raw_args[2]);
            Cli::TaskOk(raw_args[2]);
            return 0;
        } else if (sub == "enable" && raw_args.size() >= 3) {
            core.repositories().enable(raw_args[2]);
            Cli::TaskBegin("repo enable", raw_args[2]);
            Cli::TaskOk("enabled");
            return 0;
        } else if (sub == "disable" && raw_args.size() >= 3) {
            core.repositories().disable(raw_args[2]);
            Cli::TaskBegin("repo disable", raw_args[2]);
            Cli::TaskOk("disabled");
            return 0;
        }
    }

    // ---- Extensions & Plugins ----
    if (command == "ext") {
        if (raw_args.size() < 2) {
            Cli::Error("usage: lunar ext <list|load|run> ...");
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            std::vector<std::vector<std::string>> rows;
            for (const auto& ext : core.extensions().list_extensions()) {
                rows.push_back({ext.name, ext.version, ext.description, ext.file_path});
            }
            Cli::PrintTable({"NAME", "VERSION", "DESCRIPTION", "PATH"}, rows);
            return 0;
        } else if (sub == "load" && raw_args.size() >= 3) {
            std::string so_path = raw_args[2];
            Cli::TaskBegin("ext load", so_path);
            if (core.extensions().load_plugin(so_path)) {
                Cli::TaskOk(so_path);
                return 0;
            }
            Cli::TaskFail("failed to load plugin");
            return 1;
        } else if (sub == "run" && raw_args.size() >= 3) {
            std::string name = raw_args[2];
            std::vector<std::string> ext_args(raw_args.begin() + 3, raw_args.end());
            Cli::TaskBegin("ext run", name);
            if (core.extensions().execute_operation(name, ext_args)) {
                Cli::TaskOk(name);
                return 0;
            }
            Cli::TaskFail("extension failed");
            return 1;
        }
    }

    Cli::Error("unknown command: " + command);
    return 1;
}
