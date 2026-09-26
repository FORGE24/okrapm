#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <fstream>
#include "okrapmlib/lunar_core.h"
#include "okrapmlib/artifact_engine.h"
#include "okrapmlib/pipeline_engine.h"

using namespace okrapm;

void print_banner() {
    std::cout << "\033[1;32m"
              << "   ____   __ __  ___     ____   ____   __  ___\n"
              << "  / __ \\ / //_/ /   |   / __ \\ / __ \\ /  |/  /\n"
              << " / / / // ,<   / /| |  / /_/ // /_/ // /|_/ / \n"
              << "/ /_/ // /| | / ___ | / _, _// ____// /  / /  \n"
              << "\\____//_/ |_|/_/  |_|/_/ |_|/_/    /_/  /_/   \n"
              << "\033[0m\n"
              << "Lunar Package & System State Manager v1.0.0 (Okra Rolling Linux)\n\n";
}

void print_help() {
    print_banner();
    std::cout << "Usage: lunar [options] <command> [arguments...]\n\n"
              << "Options:\n"
              << "  -r, --root <dir>          Specify custom state and data root directory\n\n"
              << "Commands:\n"
              << "  install <ref...>          Install package(s), group(s) (#group), artifact(s) (.oaa)\n"
              << "  install-group <name>      Install a group of objects\n"
              << "  download <ref...>         Download package artifact(s) without installing\n"
              << "  remove <ref...>           Remove package(s) or object(s)\n"
              << "  purge <ref...>            Remove package(s) and purge configuration\n"
              << "  update [ref...]           Rolling update for system or objects\n"
              << "  upgradle <target...>      System-level baseline upgrade\n"
              << "  sync [target...]          Sync repositories or system objects\n"
              << "  plan <command> <ref...>   Preview transaction plan without applying\n"
              << "\n"
              << "Pipeline & Stream Processing:\n"
              << "  pipe '<expr>'             Execute object stream pipeline (e.g. 'find \"gnu.*\" | where outdated | update')\n"
              << "\n"
              << "Artifact & Packaging (.oaa / .okra):\n"
              << "  build <dir> [out_file]    Build an artifact package from directory\n"
              << "  artifact inspect <file>   Inspect metadata and files in artifact\n"
              << "  artifact verify <file>   Verify package checksum and archive integrity\n"
              << "  artifact extract <f> <d>  Extract artifact archive to destination directory\n"
              << "\n"
              << "Query & Inspection:\n"
              << "  search <query>            Search objects by keyword\n"
              << "  find <pattern>            Find objects by glob pattern (e.g. \"gnu.*\")\n"
              << "  list                      List all installed objects\n"
              << "  status                    Display current system state summary\n"
              << "  info <ref>                Show detailed object information\n"
              << "  members <#group>          Expand group and list its member packages\n"
              << "\n"
              << "System State & Transactions:\n"
              << "  transaction list          List recent transaction history\n"
              << "  transaction show <id>     Show transaction details\n"
              << "  snapshot list             List system snapshots\n"
              << "  snapshot create [desc]    Create a new system snapshot\n"
              << "  rollback [snapshot_id]    Rollback system state to a previous snapshot\n"
              << "\n"
              << "Repository Management:\n"
              << "  repo list                 List configured repositories\n"
              << "  repo add <name> <url> [t] Add a repository (local or remote)\n"
              << "  repo remove <name>        Remove a repository\n"
              << "  repo enable <name>        Enable a repository\n"
              << "  repo disable <name>       Disable a repository\n"
              << "\n"
              << "Extensions & Plugins:\n"
              << "  ext list                  List installed extensions and plugins\n"
              << "  ext load <path.so>        Dynamically load a shared library plugin\n"
              << "  ext run <name> [args...]  Execute an extension operation\n\n";
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
            std::cerr << "Error: 'lunar pipe' requires a pipeline expression (e.g. 'find \"gnu.*\" | where outdated | update')\n";
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
            std::cerr << "Pipeline Error: " << pipe_res.error_message << "\n";
            return 1;
        }
        std::cout << pipe_res.output;
        return 0;
    }

    // ---- Artifact Packaging (build) ----
    if (command == "build") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar build <source_dir> [output_file.oaa]\n";
            return 1;
        }
        std::string src_dir = raw_args[1];
        ArtifactBuilder::BuildOptions opts;
        if (raw_args.size() >= 3) {
            opts.output_path = raw_args[2];
        }
        std::cout << ":: Building artifact from '" << src_dir << "'...\n";
        auto built = ArtifactBuilder::build(src_dir, opts);
        if (built) {
            std::cout << ":: Artifact built successfully: " << *built << "\n";
            return 0;
        } else {
            std::cerr << "Error: Failed to build artifact from " << src_dir << "\n";
            return 1;
        }
    }

    // ---- Artifact Verification (verify) ----
    if (command == "verify") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar verify <file.oaa>\n";
            return 1;
        }
        const std::string& file = raw_args[1];
        auto meta = ArtifactExtractor::inspect(file);
        if (!meta) {
            std::cerr << "Error: Failed to verify artifact " << file << "\n";
            return 1;
        }
        std::ifstream sidecar(file + ".sha256");
        if (sidecar) {
            std::string expected;
            sidecar >> expected;
            if (expected.empty() || expected != ArtifactExtractor::calculate_sha256(file)) {
                std::cerr << "Error: SHA256 verification failed: " << file << "\n";
                return 1;
            }
        }
        std::cout << ":: " << file << " is VALID (SHA256 "
                  << ArtifactExtractor::calculate_sha256(file) << ").\n";
        return 0;
    }

    // ---- Artifact Sub-commands (artifact) ----
    if (command == "artifact") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar artifact <inspect <file> | extract <file> <dest>>\n";
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "verify" && raw_args.size() >= 3) {
            std::string file = raw_args[2];
            auto meta = ArtifactExtractor::inspect(file);
            if (!meta) {
                std::cerr << "Error: Artifact verification failed: " << file << "\n";
                return 1;
            }
            std::ifstream sidecar(file + ".sha256");
            if (sidecar) {
                std::string expected;
                sidecar >> expected;
                if (expected.empty() || expected != ArtifactExtractor::calculate_sha256(file)) {
                    std::cerr << "Error: Artifact SHA256 verification failed: " << file << "\n";
                    return 1;
                }
            }
            if (!meta->checksum.empty() && meta->checksum != ArtifactExtractor::calculate_sha256(file)) {
                std::cerr << "Error: Embedded artifact SHA256 verification failed: " << file << "\n";
                return 1;
            }
            std::cout << ":: " << file << " is VALID (SHA256 " << ArtifactExtractor::calculate_sha256(file) << ").\n";
            return 0;
        } else if (sub == "inspect" && raw_args.size() >= 3) {
            std::string file = raw_args[2];
            auto meta = ArtifactExtractor::inspect(file);
            if (!meta) {
                std::cerr << "Error: Failed to inspect artifact " << file << "\n";
                return 1;
            }
            std::cout << "Artifact:      " << file << "\n"
                      << "Namespace:     " << meta->ns << "\n"
                      << "Name:          " << meta->name << "\n"
                      << "Version:       " << meta->version.to_string() << "\n"
                      << "Architecture:  " << meta->architecture << "\n"
                      << "Description:   " << meta->description << "\n"
                      << "Maintainer:    " << meta->maintainer << "\n"
                      << "SHA256:        " << meta->checksum << "\n"
                      << "Size:          " << (meta->download_size / 1024) << " KB\n";
            if (!meta->dependencies.empty()) {
                std::cout << "Dependencies:  ";
                for (const auto& dep : meta->dependencies) std::cout << dep << " ";
                std::cout << "\n";
            }
            return 0;
        } else if (sub == "extract" && raw_args.size() >= 4) {
            std::string file = raw_args[2];
            std::string dest = raw_args[3];
            std::cout << ":: Extracting '" << file << "' to '" << dest << "'...\n";
            if (ArtifactExtractor::extract(file, dest, /*verbose=*/true)) {
                std::cout << ":: Extraction complete.\n";
                return 0;
            } else {
                std::cerr << "Error: Extraction failed.\n";
                return 1;
            }
        }
    }

    // ---- Plan 预览模式 ----
    if (command == "plan") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: 'lunar plan' requires a sub-command (e.g., install, remove, update)\n";
            return 1;
        }
        std::string sub_cmd = raw_args[1];
        std::vector<std::string> args(raw_args.begin() + 2, raw_args.end());
        args = expand_stdin_targets(args);

        if (sub_cmd == "install") {
            auto res = core.install(args, /*plan_only=*/true);
            if (!res.success && res.transaction.operations().empty()) {
                std::cerr << "Plan error: " << res.error_message << "\n";
                return 1;
            }
            std::cout << res.transaction.to_string() << "\n";
            return 0;
        } else if (sub_cmd == "remove") {
            auto res = core.remove(args, /*purge=*/false, /*plan_only=*/true);
            if (!res.success && res.transaction.operations().empty()) {
                std::cerr << "Plan error: " << res.error_message << "\n";
                return 1;
            }
            std::cout << res.transaction.to_string() << "\n";
            return 0;
        } else if (sub_cmd == "update") {
            auto res = core.update(args, /*plan_only=*/true);
            if (!res.success && res.transaction.operations().empty()) {
                std::cerr << "Plan error: " << res.error_message << "\n";
                return 1;
            }
            std::cout << res.transaction.to_string() << "\n";
            return 0;
        } else if (sub_cmd == "upgradle") {
            auto res = core.upgradle(args, /*plan_only=*/true);
            if (!res.success && res.transaction.operations().empty()) {
                std::cerr << "Plan error: " << res.error_message << "\n";
                return 1;
            }
            std::cout << res.transaction.to_string() << "\n";
            return 0;
        } else {
            std::cerr << "Unknown sub-command for plan: " << sub_cmd << "\n";
            return 1;
        }
    }

    // ---- Install ----
    if (command == "install") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: No packages or objects specified to install.\n";
            return 1;
        }
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        std::cout << ":: Resolving dependencies for " << targets.size() << " target(s)...\n";
        auto res = core.install(targets);
        if (res.success) {
            std::cout << res.transaction.to_string() << "\n";
            std::cout << ":: Transaction committed successfully.\n";
            return 0;
        } else {
            std::cerr << "Error: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Install Group ----
    if (command == "install-group") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: Group name required.\n";
            return 1;
        }
        std::string group_ref = raw_args[1];
        if (group_ref.empty() || group_ref[0] != '#') {
            group_ref = "#" + group_ref;
        }
        auto res = core.install({group_ref});
        if (res.success) {
            std::cout << res.transaction.to_string() << "\n";
            std::cout << ":: Group transaction committed successfully.\n";
            return 0;
        } else {
            std::cerr << "Error: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Download (Fetch only) ----
    if (command == "download") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: No packages or objects specified to download.\n";
            return 1;
        }
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        std::cout << ":: Downloading artifact(s) for " << targets.size() << " target(s)...\n";
        auto res = core.download(targets);
        if (res.success) {
            std::cout << ":: Download completed successfully:\n";
            for (const auto& p : res.downloaded_paths) {
                std::cout << "  - " << p << "\n";
            }
            return 0;
        } else {
            std::cerr << "Download Error: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Remove / Purge ----
    if (command == "remove" || command == "purge") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: No objects specified to remove.\n";
            return 1;
        }
        bool is_purge = (command == "purge");
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        auto res = core.remove(targets, is_purge);
        if (res.success) {
            std::cout << res.transaction.to_string() << "\n";
            std::cout << ":: Remove transaction committed successfully.\n";
            return 0;
        } else {
            std::cerr << "Error: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Update (Rolling update) ----
    if (command == "update") {
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());
        targets = expand_stdin_targets(targets);

        std::cout << ":: Checking rolling updates...\n";
        auto res = core.update(targets);
        if (res.success) {
            if (res.transaction.operations().empty()) {
                std::cout << ":: System is already up to date. S(t) is current.\n";
            } else {
                std::cout << res.transaction.to_string() << "\n";
                std::cout << ":: System state updated successfully.\n";
            }
            return 0;
        } else {
            std::cerr << "Error: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Upgradle (System-level baseline upgrade) ----
    if (command == "upgradle") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: Target system object required for upgradle (e.g. okra.systemversion).\n";
            return 1;
        }
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());

        std::cout << ":: Initiating system baseline upgrade (upgradle)...\n";
        auto res = core.upgradle(targets);
        if (res.success) {
            std::cout << res.transaction.to_string() << "\n";
            std::cout << ":: System baseline successfully shifted.\n";
            return 0;
        } else {
            std::cerr << "Error: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Sync ----
    if (command == "sync") {
        std::vector<std::string> targets(raw_args.begin() + 1, raw_args.end());

        std::cout << ":: Synchronizing world state...\n";
        auto res = core.sync(targets);
        if (res.success) {
            std::cout << ":: Synchronization complete.\n";
            return 0;
        } else {
            std::cerr << "Sync failed: " << res.error_message << "\n";
            return 1;
        }
    }

    // ---- Find (Pattern query) ----
    if (command == "find") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: Pattern required (e.g. \"gnu.*\" or \"kde.*\")\n";
            return 1;
        }
        std::string pattern = raw_args[1];
        auto col = core.find(pattern);
        std::cout << "Collection<Object> (" << col.count() << " items matching \"" << pattern << "\"):\n";
        for (const auto& obj : col.to_vector()) {
            std::cout << "  " << std::left << std::setw(28) << obj.ref_string()
                      << " " << obj.description() << "\n";
        }
        return 0;
    }

    // ---- Search ----
    if (command == "search") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: Keyword required.\n";
            return 1;
        }
        std::string query = raw_args[1];
        auto col = core.search(query);
        std::cout << "Collection<Object> (" << col.count() << " results for \"" << query << "\"):\n";
        for (const auto& obj : col.to_vector()) {
            std::cout << "  " << std::left << std::setw(28) << obj.ref_string()
                      << " " << obj.description() << "\n";
        }
        return 0;
    }

    // ---- List Installed ----
    if (command == "list") {
        auto col = core.list_installed();
        std::cout << "Installed Objects (" << col.count() << " total):\n";
        for (const auto& obj : col.to_vector()) {
            std::cout << "  " << std::left << std::setw(28) << obj.ref_string()
                      << " [v" << obj.version().to_string() << "] ("
                      << obj.repository() << ")\n";
        }
        return 0;
    }

    // ---- Info ----
    if (command == "info") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: Object ref required.\n";
            return 1;
        }
        auto obj = core.info(raw_args[1]);
        if (!obj) {
            std::cerr << "Object not found: " << raw_args[1] << "\n";
            return 1;
        }
        std::cout << "Object:       " << obj->ref_string() << "\n"
                  << "Namespace:    " << obj->ns() << "\n"
                  << "Name:         " << obj->name() << "\n"
                  << "Version:      " << obj->version().to_string() << "\n"
                  << "Type:         " << Object::type_name(obj->type()) << "\n"
                  << "Repository:   " << obj->repository() << "\n"
                  << "Description:  " << obj->description() << "\n"
                  << "Download Size:" << (obj->download_size() / 1024) << " KB\n"
                  << "Install Size: " << (obj->installed_size() / 1024) << " KB\n";
        if (!obj->dependencies().empty()) {
            std::cout << "Dependencies: ";
            for (const auto& dep : obj->dependencies()) std::cout << dep << " ";
            std::cout << "\n";
        }
        return 0;
    }

    // ---- Members (Group expand) ----
    if (command == "members") {
        if (raw_args.size() < 2) {
            std::cerr << "Error: Group ref required (e.g. #kde.kde-desktop)\n";
            return 1;
        }
        std::string group_ref = raw_args[1];
        if (!group_ref.empty() && group_ref[0] == '#') group_ref = group_ref.substr(1);
        auto obj = core.info(group_ref);
        if (!obj) {
            std::cerr << "Group not found: " << raw_args[1] << "\n";
            return 1;
        }
        std::cout << "Group " << raw_args[1] << " members (" << obj->dependencies().size() << " objects):\n";
        for (const auto& mem : obj->dependencies()) {
            std::cout << "  - " << mem << "\n";
        }
        return 0;
    }

    // ---- Status ----
    if (command == "status") {
        auto st = core.status();
        std::cout << "=== Lunar System State Summary ===\n"
                  << "State ID:         #" << st.state_id << "\n"
                  << "System Baseline:  " << st.system_version << "\n"
                  << "Installed Objects:" << st.installed_count << "\n"
                  << "Outdated Objects: " << st.outdated_count << "\n"
                  << "Active Repos:     ";
        for (const auto& r : st.repositories) std::cout << r << " ";
        std::cout << "\n";
        return 0;
    }

    // ---- Transaction ----
    if (command == "transaction") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar transaction <list|show <id>>\n";
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            auto history = core.transaction_history();
            std::cout << "Transaction History (" << history.size() << " transactions):\n";
            for (const auto& txn : history) {
                std::cout << "  #" << txn.id() << "  [" << Transaction::state_name(txn.state()) << "] "
                          << txn.description() << " (" << txn.operations().size() << " ops)\n";
            }
            return 0;
        } else if (sub == "show" && raw_args.size() >= 3) {
            uint64_t id = std::stoull(raw_args[2]);
            auto txn = core.get_transaction(id);
            if (!txn) {
                std::cerr << "Transaction #" << id << " not found\n";
                return 1;
            }
            std::cout << txn->to_string() << "\n";
            return 0;
        }
    }

    // ---- Snapshot & Rollback ----
    if (command == "snapshot") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar snapshot <list|create [desc]>\n";
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            auto snaps = core.snapshots().list();
            std::cout << "System Snapshots (" << snaps.size() << " total):\n";
            for (const auto& snap : snaps) {
                std::cout << "  " << snap.to_string() << "\n";
            }
            return 0;
        } else if (sub == "create") {
            std::string desc = (raw_args.size() >= 3) ? raw_args[2] : "Manual Snapshot";
            auto snap = core.create_snapshot(desc);
            std::cout << ":: Created Snapshot " << snap.to_string() << "\n";
            return 0;
        }
    }

    if (command == "rollback") {
        if (raw_args.size() < 2) {
            auto snaps = core.snapshots().list();
            if (snaps.empty()) {
                std::cerr << "No snapshots available for rollback.\n";
                return 1;
            }
            uint64_t latest_id = snaps.back().id;
            std::cout << ":: Rolling back to most recent snapshot #" << latest_id << "...\n";
            if (core.rollback(latest_id)) {
                std::cout << ":: Rollback completed successfully.\n";
                return 0;
            } else {
                std::cerr << ":: Rollback failed.\n";
                return 1;
            }
        } else {
            std::string snap_str = raw_args[1];
            if (!snap_str.empty() && snap_str[0] == '#') snap_str = snap_str.substr(1);
            uint64_t snap_id = std::stoull(snap_str);
            std::cout << ":: Rolling back to snapshot #" << snap_id << "...\n";
            if (core.rollback(snap_id)) {
                std::cout << ":: Rollback to snapshot #" << snap_id << " completed successfully.\n";
                return 0;
            } else {
                std::cerr << ":: Rollback failed.\n";
                return 1;
            }
        }
    }

    // ---- Repository ----
    if (command == "repo") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar repo <list|add|remove|enable|disable>\n";
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            std::cout << "Configured Repositories:\n";
            for (const auto& repo : core.repositories().list()) {
                std::cout << "  " << std::left << std::setw(15) << repo->name()
                          << " [" << (repo->enabled() ? "enabled" : "disabled") << "]\n";
            }
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
            std::cout << ":: Repository '" << name << "' (" << type << ") added: " << url << "\n";
            return 0;
        } else if (sub == "add" && raw_args.size() == 3) {
            std::string name = raw_args[2];
            core.repositories().add(std::make_shared<LocalRepository>(name, data_dir + "/repos/" + name + "/repo.db"));
            std::cout << ":: Local repository '" << name << "' added.\n";
            return 0;
        } else if (sub == "remove" && raw_args.size() >= 3) {
            core.repositories().remove(raw_args[2]);
            std::cout << ":: Repository '" << raw_args[2] << "' removed.\n";
            return 0;
        } else if (sub == "enable" && raw_args.size() >= 3) {
            core.repositories().enable(raw_args[2]);
            std::cout << ":: Repository '" << raw_args[2] << "' enabled.\n";
            return 0;
        } else if (sub == "disable" && raw_args.size() >= 3) {
            core.repositories().disable(raw_args[2]);
            std::cout << ":: Repository '" << raw_args[2] << "' disabled.\n";
            return 0;
        }
    }

    // ---- Extensions & Plugins ----
    if (command == "ext") {
        if (raw_args.size() < 2) {
            std::cerr << "Usage: lunar ext <list|load <path.so>|run <name> [args...]>\n";
            return 1;
        }
        std::string sub = raw_args[1];
        if (sub == "list") {
            std::cout << "Installed Extensions & Plugins:\n";
            for (const auto& ext : core.extensions().list_extensions()) {
                std::cout << "  " << std::left << std::setw(20) << ext.name
                          << " (v" << ext.version << ") - " << ext.description;
                if (!ext.file_path.empty()) {
                    std::cout << " [" << ext.file_path << "]";
                }
                std::cout << "\n";
            }
            return 0;
        } else if (sub == "load" && raw_args.size() >= 3) {
            std::string so_path = raw_args[2];
            if (core.extensions().load_plugin(so_path)) {
                std::cout << ":: Plugin '" << so_path << "' loaded successfully.\n";
                return 0;
            } else {
                std::cerr << "Error: Failed to load plugin " << so_path << "\n";
                return 1;
            }
        } else if (sub == "run" && raw_args.size() >= 3) {
            std::string name = raw_args[2];
            std::vector<std::string> ext_args(raw_args.begin() + 3, raw_args.end());
            if (core.extensions().execute_operation(name, ext_args)) {
                return 0;
            } else {
                std::cerr << "Extension execution failed: " << name << "\n";
                return 1;
            }
        }
    }

    std::cerr << "Unknown command: " << command << ". Run 'lunar --help' for usage.\n";
    return 1;
}
