#include "okrapmlib/artifact_engine.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <array>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace okrapm {

static std::string trim(const std::string& str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static std::string exec_command(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

std::string ArtifactMetadata::serialize_yaml() const {
    std::ostringstream oss;
    oss << "name: " << name << "\n";
    oss << "namespace: " << ns << "\n";
    oss << "version: " << version.to_string() << "\n";
    if (!description.empty()) oss << "description: \"" << description << "\"\n";
    if (!architecture.empty()) oss << "architecture: " << architecture << "\n";
    if (!maintainer.empty()) oss << "maintainer: \"" << maintainer << "\"\n";
    if (installed_size > 0) oss << "installed_size: " << installed_size << "\n";
    if (!checksum.empty()) oss << "checksum: " << checksum << "\n";

    if (!dependencies.empty()) {
        oss << "dependencies:\n";
        for (const auto& dep : dependencies) {
            oss << "  - " << dep << "\n";
        }
    }

    if (!files.empty()) {
        oss << "files:\n";
        for (const auto& f : files) {
            oss << "  - " << f << "\n";
        }
    }
    return oss.str();
}

std::optional<ArtifactMetadata> ArtifactMetadata::parse_yaml(const std::string& content) {
    std::istringstream iss(content);
    std::string line;
    ArtifactMetadata meta;
    std::string current_list_key;

    while (std::getline(iss, line)) {
        std::string raw = line;
        auto comment_pos = raw.find('#');
        if (comment_pos != std::string::npos) {
            raw = raw.substr(0, comment_pos);
        }
        std::string trimmed = trim(raw);
        if (trimmed.empty()) continue;

        if (trimmed.rfind("- ", 0) == 0) {
            std::string val = trim(trimmed.substr(2));
            if (!val.empty()) {
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                if (current_list_key == "dependencies") {
                    meta.dependencies.push_back(val);
                } else if (current_list_key == "files") {
                    meta.files.push_back(val);
                }
            }
            continue;
        }

        auto colon_pos = trimmed.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, colon_pos));
        std::string val = trim(trimmed.substr(colon_pos + 1));
        if (!val.empty() && val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        if (key == "dependencies" || key == "deps" || key == "files") {
            current_list_key = (key == "deps") ? "dependencies" : key;
            continue;
        } else {
            current_list_key = "";
        }

        if (key == "name") {
            meta.name = val;
        } else if (key == "namespace" || key == "ns") {
            meta.ns = val;
        } else if (key == "version") {
            auto v = Version::parse(val);
            if (v) meta.version = *v;
        } else if (key == "description" || key == "desc") {
            meta.description = val;
        } else if (key == "architecture" || key == "arch") {
            meta.architecture = val;
        } else if (key == "maintainer") {
            meta.maintainer = val;
        } else if (key == "checksum" || key == "sha256") {
            meta.checksum = val;
        } else if (key == "installed_size" || key == "size") {
            try { meta.installed_size = std::stoull(val); } catch (...) {}
        }
    }

    if (meta.name.empty()) return std::nullopt;
    return meta;
}

std::optional<ArtifactMetadata> ArtifactMetadata::load_from_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return parse_yaml(content);
}

Object ArtifactMetadata::to_object() const {
    Object obj(ns, name, version, ObjectType::Package);
    obj.set_description(description);
    obj.set_dependencies(dependencies);
    obj.set_files(files);
    obj.set_installed_size(installed_size > 0 ? installed_size : 1024 * 1024 * 10);
    obj.set_download_size(download_size > 0 ? download_size : 1024 * 1024 * 5);
    obj.set_repository("local-artifact");
    return obj;
}

std::optional<std::string> ArtifactBuilder::build(const std::string& source_dir) {
    BuildOptions opts;
    return build(source_dir, opts);
}

std::optional<std::string> ArtifactBuilder::build(const std::string& source_dir,
                                                  const BuildOptions& options) {
    if (!fs::exists(source_dir) || !fs::is_directory(source_dir)) {
        std::cerr << "ArtifactBuilder: source directory not found: " << source_dir << "\n";
        return std::nullopt;
    }

    std::string meta_path = source_dir + "/meta.yaml";
    if (!fs::exists(meta_path)) {
        meta_path = source_dir + "/meta.json";
    }
    if (!fs::exists(meta_path)) {
        std::cerr << "ArtifactBuilder: meta.yaml not found in " << source_dir << "\n";
        return std::nullopt;
    }

    auto meta = ArtifactMetadata::load_from_file(meta_path);
    if (!meta) {
        std::cerr << "ArtifactBuilder: failed to parse " << meta_path << "\n";
        return std::nullopt;
    }

    // 执行 pre-build 脚本
    std::string pre_build = source_dir + "/scripts/pre-build";
    if (fs::exists(pre_build)) {
        ArtifactExtractor::execute_hook(pre_build);
    }

    std::string output_file = options.output_path;
    if (output_file.empty()) {
        output_file = meta->name + "-" + meta->version.to_string() + ".oaa";
    }

    // 打包压缩
    std::string tar_cmd;
    if (options.compression == "zstd") {
        tar_cmd = "tar --zstd -cf \"" + output_file + "\" -C \"" + source_dir + "\" . 2>/dev/null";
        int ret = std::system(tar_cmd.c_str());
        if (ret != 0) {
            // fallback to zstd / gzip
            tar_cmd = "tar -czf \"" + output_file + "\" -C \"" + source_dir + "\" . 2>/dev/null";
            ret = std::system(tar_cmd.c_str());
            if (ret != 0) {
                tar_cmd = "tar -cf \"" + output_file + "\" -C \"" + source_dir + "\" .";
                std::system(tar_cmd.c_str());
            }
        }
    } else if (options.compression == "xz") {
        tar_cmd = "tar -cJf \"" + output_file + "\" -C \"" + source_dir + "\" . 2>/dev/null";
        std::system(tar_cmd.c_str());
    } else if (options.compression == "gzip") {
        tar_cmd = "tar -czf \"" + output_file + "\" -C \"" + source_dir + "\" . 2>/dev/null";
        std::system(tar_cmd.c_str());
    } else {
        tar_cmd = "tar -cf \"" + output_file + "\" -C \"" + source_dir + "\" .";
        std::system(tar_cmd.c_str());
    }

    if (!fs::exists(output_file)) {
        std::cerr << "ArtifactBuilder: archive generation failed\n";
        return std::nullopt;
    }

    // 执行 post-build 脚本
    std::string post_build = source_dir + "/scripts/post-build";
    if (fs::exists(post_build)) {
        ArtifactExtractor::execute_hook(post_build);
    }

    return output_file;
}

std::optional<ArtifactMetadata> ArtifactExtractor::inspect(const std::string& archive_path) {
    if (!fs::exists(archive_path)) return std::nullopt;

    // 使用 tar 查看/解出 meta.yaml 内容
    std::string cmd = "tar --wildcards -xf \"" + archive_path + "\" \"*meta.yaml\" -O 2>/dev/null || "
                      "tar --zstd --wildcards -xf \"" + archive_path + "\" \"*meta.yaml\" -O 2>/dev/null || "
                      "tar -xzf \"" + archive_path + "\" ./meta.yaml -O 2>/dev/null || "
                      "tar -xf \"" + archive_path + "\" meta.yaml -O 2>/dev/null";

    std::string meta_content = exec_command(cmd);
    if (meta_content.empty()) {
        // 尝试 meta.json
        cmd = "tar --wildcards -xf \"" + archive_path + "\" \"*meta.json\" -O 2>/dev/null || "
              "tar --zstd --wildcards -xf \"" + archive_path + "\" \"*meta.json\" -O 2>/dev/null || "
              "tar -xzf \"" + archive_path + "\" ./meta.json -O 2>/dev/null || "
              "tar -xf \"" + archive_path + "\" meta.json -O 2>/dev/null";
        meta_content = exec_command(cmd);
    }

    if (meta_content.empty()) return std::nullopt;

    auto meta = ArtifactMetadata::parse_yaml(meta_content);
    if (meta) {
        std::error_code ec;
        meta->download_size = fs::file_size(archive_path, ec);
        meta->checksum = calculate_sha256(archive_path);
    }
    return meta;
}

bool ArtifactExtractor::extract(const std::string& archive_path,
                                const std::string& target_dir,
                                bool verbose) {
    if (!fs::exists(archive_path)) return false;

    std::error_code ec;
    fs::create_directories(target_dir, ec);
    std::string v_flag = verbose ? "v" : "";
    std::string cmd = "tar -x" + v_flag + "f \"" + archive_path + "\" -C \"" + target_dir + "\" 2>/dev/null || "
                      "tar --zstd -x" + v_flag + "f \"" + archive_path + "\" -C \"" + target_dir + "\" 2>/dev/null || "
                      "tar -xzf \"" + archive_path + "\" -C \"" + target_dir + "\" 2>/dev/null || "
                      "tar -xJf \"" + archive_path + "\" -C \"" + target_dir + "\" 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

int ArtifactExtractor::execute_hook(const std::string& script_path,
                                    const std::vector<std::string>& args) {
    if (!fs::exists(script_path)) return 0;

    // 确保有可执行权限
    chmod(script_path.c_str(), 0755);

    std::string cmd = "\"" + script_path + "\"";
    for (const auto& arg : args) {
        cmd += " \"" + arg + "\"";
    }
    return std::system(cmd.c_str());
}

std::string ArtifactExtractor::calculate_sha256(const std::string& file_path) {
    if (!fs::exists(file_path)) return "";
    std::string cmd = "sha256sum \"" + file_path + "\" 2>/dev/null";
    std::string out = exec_command(cmd);
    auto space_pos = out.find(' ');
    if (space_pos != std::string::npos) {
        return out.substr(0, space_pos);
    }
    return trim(out);
}

} // namespace okrapm
