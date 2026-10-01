#include "oaa_core.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <array>
#include <cstdlib>

namespace fs = std::filesystem;

namespace oaa {

static std::string trim(const std::string& str) {
    auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static std::string exec_cmd(const std::string& cmd) {
    std::array<char, 512> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

std::string OaaMeta::to_yaml() const {
    std::ostringstream oss;
    oss << "name: " << name << "\n"
        << "namespace: " << ns << "\n"
        << "version: " << version << "\n";
    if (!description.empty()) oss << "description: \"" << description << "\"\n";
    oss << "architecture: " << architecture << "\n";
    if (!maintainer.empty()) oss << "maintainer: \"" << maintainer << "\"\n";
    if (installed_size > 0) oss << "installed_size: " << installed_size << "\n";
    if (!checksum.empty()) oss << "checksum: " << checksum << "\n";

    if (!dependencies.empty()) {
        oss << "dependencies:\n";
        for (const auto& dep : dependencies) oss << "  - " << dep << "\n";
    }

    if (!files.empty()) {
        oss << "files:\n";
        for (const auto& f : files) oss << "  - " << f << "\n";
    }
    return oss.str();
}

std::optional<OaaMeta> OaaMeta::from_yaml_string(const std::string& yaml_str) {
    std::istringstream iss(yaml_str);
    std::string line;
    OaaMeta meta;
    std::string current_list_key;

    while (std::getline(iss, line)) {
        auto comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        line = trim(line);
        if (line.empty()) continue;

        if (line.rfind("- ", 0) == 0) {
            std::string val = trim(line.substr(2));
            if (!val.empty() && (val.front() == '"' || val.front() == '\'')) val = val.substr(1, val.length() - 2);
            if (current_list_key == "dependencies") meta.dependencies.push_back(val);
            else if (current_list_key == "files") meta.files.push_back(val);
            continue;
        }

        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = trim(line.substr(0, colon));
            std::string val = trim(line.substr(colon + 1));
            if (!val.empty() && (val.front() == '"' || val.front() == '\'')) val = val.substr(1, val.length() - 2);

            if (val.empty()) {
                current_list_key = key;
            } else {
                current_list_key.clear();
                if (key == "name") meta.name = val;
                else if (key == "namespace") meta.ns = val;
                else if (key == "version") meta.version = val;
                else if (key == "description") meta.description = val;
                else if (key == "architecture") meta.architecture = val;
                else if (key == "maintainer") meta.maintainer = val;
                else if (key == "installed_size") {
                    try { meta.installed_size = std::stoull(val); } catch (...) {}
                } else if (key == "checksum") meta.checksum = val;
            }
        }
    }
    if (meta.name.empty()) return std::nullopt;
    return meta;
}

std::optional<OaaMeta> OaaMeta::from_yaml_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::nullopt;
    std::stringstream ss;
    ss << ifs.rdbuf();
    return from_yaml_string(ss.str());
}

std::string OaaCore::calculate_sha256(const std::string& file_path) {
    if (!fs::exists(file_path)) return "";
    std::string out = exec_cmd("sha256sum \"" + file_path + "\" 2>/dev/null | awk '{print $1}'");
    return trim(out);
}

bool OaaCore::init_skeleton(const std::string& dir, const OaaMeta& initial_meta) {
    try {
        fs::create_directories(dir + "/rootfs/usr/bin");
        fs::create_directories(dir + "/rootfs/usr/lib");
        fs::create_directories(dir + "/rootfs/etc");
        fs::create_directories(dir + "/scripts");

        std::ofstream meta_out(dir + "/meta.yaml");
        meta_out << initial_meta.to_yaml();
        meta_out.close();

        std::ofstream pre_build(dir + "/scripts/pre-build");
        pre_build << "#!/bin/sh\necho \"[pre-build] building " << initial_meta.name << "\"\n";
        pre_build.close();
        fs::permissions(dir + "/scripts/pre-build", fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);

        std::ofstream post_build(dir + "/scripts/post-build");
        post_build << "#!/bin/sh\necho \"[post-build] completed " << initial_meta.name << "\"\n";
        post_build.close();
        fs::permissions(dir + "/scripts/post-build", fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);

        std::ofstream install_opsis(dir + "/scripts/install.opsis");
        install_opsis << "public class Package {\n"
                      << "    public void INSTALL() {\n"
                      << "        CheckUser();\n"
                      << "        InstallDirectory(\"rootfs\", \"/\");\n"
                      << "        UpdateLdconfig();\n"
                      << "        RecordInstalled(\"" << initial_meta.name << "\", \""
                      << initial_meta.version << "\", \"meta.yaml\");\n"
                      << "    }\n"
                      << "}\n";
        install_opsis.close();

        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> OaaCore::build_package(const std::string& src_dir, const std::string& out_path, const std::string& comp) {
    if (!fs::exists(src_dir) || !fs::is_directory(src_dir)) return std::nullopt;

    auto meta = OaaMeta::from_yaml_file(src_dir + "/meta.yaml");
    if (!meta) return std::nullopt;

    std::string pre = src_dir + "/scripts/pre-build";
    if (fs::exists(pre)) {
        std::system(("cd \"" + src_dir + "\" && ./scripts/pre-build").c_str());
    }

    std::string final_out = out_path;
    if (final_out.empty()) {
        final_out = meta->name + "-" + meta->version + ".oaa";
    }

    std::string flag = "--zstd";
    if (comp == "gzip" || comp == "gz") flag = "-z";
    else if (comp == "xz") flag = "-J";
    else if (comp == "none") flag = "";

    std::string tar_cmd = "tar " + flag + " -cf \"" + final_out + "\" --exclude=./" + fs::path(final_out).filename().string() + " -C \"" + src_dir + "\" . 2>/dev/null";
    int ret = std::system(tar_cmd.c_str());
    if (ret != 0) {
        tar_cmd = "tar -czf \"" + final_out + "\" -C \"" + src_dir + "\" .";
        std::system(tar_cmd.c_str());
    }

    if (!fs::exists(final_out)) return std::nullopt;

    std::string sha = calculate_sha256(final_out);
    std::ofstream sha_file(final_out + ".sha256");
    sha_file << sha << "  " << fs::path(final_out).filename().string() << "\n";
    sha_file.close();

    std::string post = src_dir + "/scripts/post-build";
    if (fs::exists(post)) {
        std::system(("cd \"" + src_dir + "\" && ./scripts/post-build").c_str());
    }

    return final_out;
}

std::optional<OaaMeta> OaaCore::inspect_package(const std::string& pkg_path) {
    if (!fs::exists(pkg_path)) return std::nullopt;
    std::string raw = exec_cmd("tar --zstd -xf \"" + pkg_path + "\" -O ./meta.yaml 2>/dev/null || tar -xf \"" + pkg_path + "\" -O meta.yaml 2>/dev/null || tar -xzf \"" + pkg_path + "\" -O ./meta.yaml 2>/dev/null");
    if (raw.empty()) return std::nullopt;
    auto meta = OaaMeta::from_yaml_string(raw);
    return meta;
}

bool OaaCore::extract_package(const std::string& pkg_path, const std::string& target_dir) {
    if (!fs::exists(pkg_path)) return false;
    fs::create_directories(target_dir);
    std::string cmd = "tar --zstd -xf \"" + pkg_path + "\" -C \"" + target_dir + "\" 2>/dev/null || tar -xzf \"" + pkg_path + "\" -C \"" + target_dir + "\" 2>/dev/null || tar -xf \"" + pkg_path + "\" -C \"" + target_dir + "\"";
    int ret = std::system(cmd.c_str());
    return ret == 0;
}

bool OaaCore::verify_package(const std::string& pkg_path) {
    if (!fs::exists(pkg_path)) return false;
    std::string test_cmd = "tar --zstd -tf \"" + pkg_path + "\" >/dev/null 2>&1 || tar -tf \"" + pkg_path + "\" >/dev/null 2>&1";
    int ret = std::system(test_cmd.c_str());
    return ret == 0;
}

std::vector<std::string> OaaCore::list_contents(const std::string& pkg_path) {
    std::vector<std::string> res;
    if (!fs::exists(pkg_path)) return res;
    std::string raw = exec_cmd("tar --zstd -tf \"" + pkg_path + "\" 2>/dev/null || tar -tf \"" + pkg_path + "\" 2>/dev/null");
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (!line.empty()) res.push_back(line);
    }
    return res;
}

} // namespace oaa
