#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include "okrapmlib/version.h"
#include "okrapmlib/object.h"
#include "okrapmlib/object_ref.h"
#include "okrapmlib/collection.h"
#include "okrapmlib/transaction.h"
#include "okrapmlib/lunar_core.h"
#include "okrapmlib/artifact_engine.h"
#include "okrapmlib/extension_api.h"
#include "okrapmlib/pipeline_engine.h"
#include "okrapmlib/network_downloader.h"

using namespace okrapm;
namespace fs = std::filesystem;

void test_version() {
    auto v1 = Version::parse("14.2");
    assert(v1.has_value());
    assert(v1->major() == 14 && v1->minor() == 2);
    
    auto v2 = Version::parse("14.1.0");
    assert(v2.has_value());
    assert(*v1 > *v2);
    std::cout << "[PASS] test_version\n";
}

void test_object_and_collection() {
    Package gcc("gnu", "gcc", *Version::parse("14.2"), "GNU Compiler Collection");
    gcc.add_dependency("gnu.glibc");
    gcc.add_dependency("gnu.binutils");

    Package glibc("gnu", "glibc", *Version::parse("2.40"), "GNU C Library");
    Package dolphin("kde", "dolphin", *Version::parse("24.08"), "KDE File Manager");

    std::vector<Object> list = {gcc, glibc, dolphin};
    Collection<Object> col(list);

    assert(col.count() == 3);

    // Pipeline test: where namespace == gnu
    auto gnu_pkgs = col.where([](const Object& o) { return o.ns() == "gnu"; });
    assert(gnu_pkgs.count() == 2);

    // Select names
    auto names = gnu_pkgs.select<std::string>([](const Object& o) { return o.name(); });
    assert(names.count() == 2);

    std::cout << "[PASS] test_object_and_collection\n";
}

void test_object_ref() {
    auto ref1 = ObjectRef::parse("gnu.gcc@14.2");
    assert(ref1.has_value());
    assert(ref1->ns() == "gnu");
    assert(ref1->name() == "gcc");
    assert(ref1->version_str() == "14.2");
    assert(ref1->is_package());

    auto ref2 = ObjectRef::parse("#kde.kde-desktop");
    assert(ref2.has_value());
    assert(ref2->is_group());
    assert(ref2->ns() == "kde");
    assert(ref2->name() == "kde-desktop");

    auto ref3 = ObjectRef::parse("./app.oaa");
    assert(ref3.has_value());
    assert(ref3->is_artifact());

    std::cout << "[PASS] test_object_ref\n";
}

void test_lunar_core_flow() {
    std::string test_dir = "/tmp/lunar_unit_test";
    fs::remove_all(test_dir);

    LunarCore core(test_dir);

    // 1. 初始化仓库中的测试对象
    auto repo = core.repositories().get_repository("main");
    assert(repo != nullptr);
    auto* local_repo = dynamic_cast<LocalRepository*>(repo);
    assert(local_repo != nullptr);

    Package glibc("gnu", "glibc", *Version::parse("2.40"), "GNU C Library");
    glibc.set_download_size(1024 * 1024 * 10);
    glibc.set_installed_size(1024 * 1024 * 30);

    Package binutils("gnu", "binutils", *Version::parse("2.43"), "GNU binary utilities");
    binutils.add_dependency("gnu.glibc");

    Package gcc("gnu", "gcc", *Version::parse("14.2"), "GNU Compiler Collection");
    gcc.add_dependency("gnu.binutils");
    gcc.add_dependency("gnu.glibc");

    Group kde_group("kde", "kde-desktop", *Version::parse("6.1"), "KDE Desktop Environment");
    kde_group.add_member("gnu.gcc");

    local_repo->add_object(glibc);
    local_repo->add_object(binutils);
    local_repo->add_object(gcc);
    local_repo->add_object(kde_group);
    local_repo->save();

    // 2. 测试计划与安装
    auto plan_res = core.install({"gnu.gcc"}, /*plan_only=*/true);
    assert(plan_res.success);
    assert(plan_res.transaction.operations().size() == 3); // glibc, binutils, gcc

    auto install_res = core.install({"gnu.gcc"});
    assert(install_res.success);
    assert(install_res.transaction.state() == TransactionState::Committed);

    // 3. 验证已安装列表
    auto installed = core.list_installed();
    assert(installed.count() == 3);

    // 4. 验证查找
    auto find_res = core.find("gnu.*");
    assert(find_res.count() == 3);

    // 5. 测试快照
    auto snap = core.create_snapshot("Initial test snapshot");
    assert(snap.objects.size() == 3);

    // 6. 测试卸载
    auto remove_res = core.remove({"gnu.gcc"});
    assert(remove_res.success);
    assert(core.list_installed().count() == 2);

    // 7. 测试回滚快照
    bool rb_ok = core.rollback(snap.id);
    assert(rb_ok);
    assert(core.list_installed().count() == 3);

    // 8. 测试 Group 安装
    auto grp_res = core.install({"#kde.kde-desktop"});
    assert(grp_res.success);

    fs::remove_all(test_dir);
    std::cout << "[PASS] test_lunar_core_flow\n";
}

void test_artifact_engine() {
    std::string test_dir = "/tmp/lunar_artifact_test";
    std::string extract_dir = "/tmp/lunar_artifact_extract";
    fs::remove_all(test_dir);
    fs::remove_all(extract_dir);

    fs::create_directories(test_dir + "/files/usr/bin");
    fs::create_directories(test_dir + "/scripts");

    // 写入 meta.yaml
    std::ofstream meta_ofs(test_dir + "/meta.yaml");
    meta_ofs << "name: hello\n"
             << "namespace: demo\n"
             << "version: 1.0.0\n"
             << "description: \"Hello World demo package\"\n"
             << "maintainer: \"Okra Team\"\n"
             << "dependencies:\n"
             << "  - gnu.glibc\n"
             << "files:\n"
             << "  - usr/bin/hello\n";
    meta_ofs.close();

    // 写入 payload 文件
    std::ofstream bin_ofs(test_dir + "/files/usr/bin/hello");
    bin_ofs << "#!/bin/sh\necho \"Hello from Lunar\"\n";
    bin_ofs.close();

    // 写入 pre-build 脚本
    std::ofstream pre_ofs(test_dir + "/scripts/pre-build");
    pre_ofs << "#!/bin/sh\nexit 0\n";
    pre_ofs.close();

    // 1. 测试构建
    ArtifactBuilder::BuildOptions opts;
    opts.output_path = "/tmp/hello-1.0.0.oaa";
    auto built_file = ArtifactBuilder::build(test_dir, opts);
    assert(built_file.has_value());
    assert(fs::exists(*built_file));

    // 2. 测试检查元数据 (inspect)
    auto meta = ArtifactExtractor::inspect(*built_file);
    assert(meta.has_value());
    assert(meta->name == "hello");
    assert(meta->ns == "demo");
    assert(meta->version.to_string() == "1.0.0");
    assert(!meta->checksum.empty());
    assert(meta->dependencies.size() == 1 && meta->dependencies[0] == "gnu.glibc");

    // 3. 测试解压 (extract)
    bool ext_ok = ArtifactExtractor::extract(*built_file, extract_dir);
    assert(ext_ok);
    assert(fs::exists(extract_dir + "/meta.yaml"));
    assert(fs::exists(extract_dir + "/files/usr/bin/hello"));

    // 4. 测试通过 LunarCore 直接安装 Artifact
    std::string core_dir = "/tmp/lunar_art_core_test";
    fs::remove_all(core_dir);
    LunarCore core(core_dir);
    auto inst_res = core.install({*built_file});
    assert(inst_res.success);
    auto inst_obj = core.info("demo.hello");
    assert(inst_obj.has_value());
    assert(inst_obj->name() == "hello");
    assert(inst_obj->ns() == "demo");

    // 5. 测试 .okra 格式包构建、元数据解析 (deps/desc) 以及通过 LunarCore 加载与安装
    std::string okra_dir = "/tmp/test_okra_pkg";
    fs::remove_all(okra_dir);
    fs::create_directories(okra_dir + "/files/usr/bin");

    std::ofstream ofs_okra_meta(okra_dir + "/meta.yaml");
    ofs_okra_meta << "name: okratool\n"
                  << "namespace: okrapm\n"
                  << "version: 2.1.0\n"
                  << "desc: \"Legacy okpm tool test\"\n"
                  << "deps:\n"
                  << "  - core.base\n"
                  << "arch: x86_64\n";
    ofs_okra_meta.close();

    std::ofstream ofs_okra_bin(okra_dir + "/files/usr/bin/okratool");
    ofs_okra_bin << "#!/bin/sh\necho okra\n";
    ofs_okra_bin.close();

    ArtifactBuilder::BuildOptions okra_opts;
    okra_opts.output_path = "/tmp/okratool-2.1.0.okra";
    auto built_okra = ArtifactBuilder::build(okra_dir, okra_opts);
    assert(built_okra.has_value());
    assert(fs::exists(*built_okra));

    auto okra_meta = ArtifactExtractor::inspect(*built_okra);
    assert(okra_meta.has_value());
    assert(okra_meta->name == "okratool");
    assert(okra_meta->ns == "okrapm");
    assert(okra_meta->version.to_string() == "2.1.0");
    assert(okra_meta->description == "Legacy okpm tool test");
    assert(okra_meta->dependencies.size() == 1 && okra_meta->dependencies[0] == "core.base");

    std::string okra_core_dir = "/tmp/lunar_okra_core_test";
    fs::remove_all(okra_core_dir);
    LunarCore okra_core(okra_core_dir);

    // 验证 okrapm 后端扩展已自动加载
    bool has_okrapm_ext = false;
    for (const auto& item : ExtensionApi::instance().list_extensions()) {
        if (item.name == "okrapm") {
            has_okrapm_ext = true;
            break;
        }
    }
    assert(has_okrapm_ext);

    std::vector<std::string> install_targets = {*built_okra};
    auto inst_okra_res = okra_core.install(install_targets);
    assert(inst_okra_res.success);
    auto inst_okra_obj = okra_core.info("okrapm.okratool");
    assert(inst_okra_obj.has_value());
    assert(inst_okra_obj->name() == "okratool");
    assert(inst_okra_obj->version().to_string() == "2.1.0");

    fs::remove_all(okra_dir);
    fs::remove(*built_okra);
    fs::remove_all(okra_core_dir);

    // 清理
    fs::remove_all(test_dir);
    fs::remove_all(extract_dir);
    fs::remove(*built_file);
    fs::remove_all(core_dir);
    std::cout << "[PASS] test_artifact_engine\n";
}

void test_extension_and_hooks() {
    auto& ext = ExtensionApi::instance();

    // 1. 注册自定义扩展操作
    bool custom_op_executed = false;
    ext.register_operation("docker-status", "Check docker subsystem status",
                           [&](const std::vector<std::string>& args) {
                               custom_op_executed = true;
                               return true;
                           });

    assert(ext.execute_operation("docker-status", {}));
    assert(custom_op_executed);

    // 2. 注册并测试生命周期 Hooks
    int pre_txn_count = 0;
    int post_txn_count = 0;
    ext.register_hook(HookType::PreTransaction, [&](const Transaction&) {
        pre_txn_count++;
    });
    ext.register_hook(HookType::PostTransaction, [&](const Transaction&) {
        post_txn_count++;
    });

    std::string core_dir = "/tmp/lunar_hook_test";
    fs::remove_all(core_dir);
    LunarCore core(core_dir);

    auto repo = dynamic_cast<LocalRepository*>(core.repositories().get_repository("main"));
    assert(repo != nullptr);
    Package test_pkg("app", "testapp", *Version::parse("1.0.0"), "Test Application");
    repo->add_object(test_pkg);
    repo->save();

    auto res = core.install({"app.testapp"});
    assert(res.success);
    assert(pre_txn_count >= 1);
    assert(post_txn_count >= 1);

    fs::remove_all(core_dir);
    std::cout << "[PASS] test_extension_and_hooks\n";
}

void test_plugin_entries() {
    auto& ext = ExtensionApi::instance();
    fs::path dir = fs::temp_directory_path() / "lunar-plugin-marks";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path legacy = dir / "legacy";
    fs::path oaabi = dir / "oaabi";
    setenv("LEGACY_PLUGIN_MARK", legacy.c_str(), 1);
    setenv("OAABI_MARK", oaabi.c_str(), 1);

    assert(ext.load_plugin(LEGACY_PLUGIN_PATH));
    {
        std::ifstream input(legacy);
        std::string text;
        std::getline(input, text);
        assert(text == "legacy");
    }

    assert(ext.load_plugin(OAABI_PLUGIN_PATH));
    {
        std::ifstream input(oaabi);
        std::string text;
        std::getline(input, text);
        assert(text == "init");
    }

    assert(!ext.load_plugin(OAABI_BAD_PLUGIN_PATH));
    {
        std::ifstream input(oaabi);
        std::string text;
        std::getline(input, text);
        assert(text == "init");
    }

    ext.unload_all();
    {
        std::ifstream input(oaabi);
        std::string init, fini;
        std::getline(input, init);
        std::getline(input, fini);
        assert(init == "init");
        assert(fini == "fini");
    }

    unsetenv("LEGACY_PLUGIN_MARK");
    unsetenv("OAABI_MARK");
    fs::remove_all(dir);
    std::cout << "[PASS] test_plugin_entries\n";
}

void test_pipeline_engine() {
    std::string core_dir = "/tmp/lunar_pipe_test";
    fs::remove_all(core_dir);
    LunarCore core(core_dir);

    auto repo = dynamic_cast<LocalRepository*>(core.repositories().get_repository("main"));
    assert(repo != nullptr);

    Package gcc("gnu", "gcc", *Version::parse("14.2"), "GNU Compiler Collection");
    Package glibc("gnu", "glibc", *Version::parse("2.40"), "GNU C Library");
    Package bash("gnu", "bash", *Version::parse("5.2"), "GNU Bourne-Again Shell");
    Package dolphin("kde", "dolphin", *Version::parse("24.08"), "KDE File Manager");

    repo->add_object(gcc);
    repo->add_object(glibc);
    repo->add_object(bash);
    repo->add_object(dolphin);
    repo->save();

    // 1. Pipeline: find "gnu.*" | where name=gcc | count
    auto r1 = PipelineEngine::execute("find \"gnu.*\" | where name=gcc | count", core);
    assert(r1.success);
    assert(r1.objects.count() == 1);

    // 2. Pipeline: find "*" | where namespace=gnu | count
    auto r2 = PipelineEngine::execute("find \"*\" | where namespace=gnu | count", core);
    assert(r2.success);
    assert(r2.objects.count() == 3);

    // 3. Pipeline: find "gnu.*" | sort name | limit 2 | count
    auto r3 = PipelineEngine::execute("find \"gnu.*\" | sort name | limit 2 | count", core);
    assert(r3.success);
    assert(r3.objects.count() == 2);

    // 4. Pipeline: find "gnu.bash" | install
    auto r4 = PipelineEngine::execute("find \"gnu.bash\" | install", core);
    assert(r4.success);
    assert(core.list_installed().count() == 1);

    // 5. Pipeline: list | inspect
    auto r5 = PipelineEngine::execute("list | inspect", core);
    assert(r5.success);
    assert(r5.objects.count() == 1);

    // 6. Pipeline: find "gnu.gcc" | plan install
    auto r6 = PipelineEngine::execute("find \"gnu.gcc\" | plan install", core);
    assert(r6.success);
    assert(r6.transaction.has_value());

    fs::remove_all(core_dir);
    std::cout << "[PASS] test_pipeline_engine\n";
}

void test_network_downloader() {
    std::string test_file = "/tmp/lunar_net_src.txt";
    std::string dest_file = "/tmp/lunar_net_dest.txt";
    fs::remove(test_file);
    fs::remove(dest_file);

    std::ofstream ofs(test_file);
    ofs << "Lunar Network Downloader Test Payload\n";
    ofs.close();

    std::string sha = NetworkDownloader::calculate_sha256(test_file);
    assert(!sha.empty());
    assert(NetworkDownloader::verify_checksum(test_file, sha));
    assert(!NetworkDownloader::verify_checksum(test_file, "wrong_hash_123456"));

    // 测试 file:// 协议下载
    DownloadOptions opts;
    opts.expected_sha256 = sha;
    auto res = NetworkDownloader::download_file("file://" + test_file, dest_file, opts);
    assert(res.success);
    assert(fs::exists(dest_file));
    assert(res.checksum == sha);

    // 测试字符串下载
    auto str_opt = NetworkDownloader::download_string("file://" + test_file);
    assert(str_opt.has_value());
    assert(*str_opt == "Lunar Network Downloader Test Payload\n");

    fs::remove(test_file);
    fs::remove(dest_file);
    std::cout << "[PASS] test_network_downloader\n";
}

void test_remote_repository_and_distribution() {
    std::string remote_server_dir = "/tmp/lunar_remote_server";
    std::string mock_pkg_dir = "/tmp/lunar_remote_pkg_src";
    std::string core_dir = "/tmp/lunar_remote_core_test";

    fs::remove_all(remote_server_dir);
    fs::remove_all(mock_pkg_dir);
    fs::remove_all(core_dir);

    fs::create_directories(remote_server_dir + "/artifacts");
    fs::create_directories(mock_pkg_dir + "/files/usr/bin");

    // 1. 创建模拟包源码并构建 .oaa
    std::ofstream meta_ofs(mock_pkg_dir + "/meta.yaml");
    meta_ofs << "name: ripgrep\n"
             << "namespace: tools\n"
             << "version: 14.1.0\n"
             << "description: \"Fast line-oriented search tool\"\n"
             << "maintainer: \"Okra Team\"\n"
             << "files:\n"
             << "  - usr/bin/rg\n";
    meta_ofs.close();

    std::ofstream bin_ofs(mock_pkg_dir + "/files/usr/bin/rg");
    bin_ofs << "#!/bin/sh\necho \"rg mock\"\n";
    bin_ofs.close();

    ArtifactBuilder::BuildOptions b_opts;
    b_opts.output_path = remote_server_dir + "/artifacts/tools.ripgrep@14.1.0.oaa";
    auto built = ArtifactBuilder::build(mock_pkg_dir, b_opts);
    assert(built.has_value());
    assert(fs::exists(*built));

    // 2. 生成远端 index.yaml
    std::ofstream idx_ofs(remote_server_dir + "/index.yaml");
    idx_ofs << "name: ripgrep\n"
            << "namespace: tools\n"
            << "version: 14.1.0\n"
            << "description: \"Fast line-oriented search tool\"\n"
            << "maintainer: \"Okra Team\"\n"
            << "files:\n"
            << "  - usr/bin/rg\n";
    idx_ofs.close();

    // 3. 测试 RemoteRepository 同步
    RemoteRepository remote_repo("community", "file://" + remote_server_dir, core_dir + "/repos/community");
    bool sync_ok = remote_repo.sync();
    assert(sync_ok);
    assert(remote_repo.list_objects().size() == 1);

    auto found_rg = remote_repo.find("tools", "ripgrep");
    assert(found_rg.has_value());
    assert(found_rg->name() == "ripgrep");
    assert(found_rg->ns() == "tools");
    assert(found_rg->version().to_string() == "14.1.0");

    // 4. 测试 fetch_artifact 下载包
    auto fetched_path = remote_repo.fetch_artifact(*found_rg);
    assert(fetched_path.has_value());
    assert(fs::exists(*fetched_path));

    // 5. 测试 LunarCore 远程安装与分发完整链路
    LunarCore core(core_dir);
    core.repositories().add(std::make_shared<RemoteRepository>(
        "community", "file://" + remote_server_dir, core_dir + "/repos/community"));

    auto sync_res = core.sync({"community"});
    assert(sync_res.success);

    // 仅下载测试
    auto dl_res = core.download({"tools.ripgrep"});
    assert(dl_res.success);
    assert(!dl_res.downloaded_paths.empty());
    assert(fs::exists(dl_res.downloaded_paths[0]));

    // 远程安装测试
    auto inst_res = core.install({"tools.ripgrep"});
    assert(inst_res.success);
    assert(core.list_installed().count() == 1);
    auto installed_rg = core.info("tools.ripgrep");
    assert(installed_rg.has_value());
    assert(installed_rg->name() == "ripgrep");

    // 清理
    fs::remove_all(remote_server_dir);
    fs::remove_all(mock_pkg_dir);
    fs::remove_all(core_dir);
    std::cout << "[PASS] test_remote_repository_and_distribution\n";
}

int main() {
    std::cout << "Running okrapm / lunar comprehensive test suite...\n";
    test_version();
    test_object_and_collection();
    test_object_ref();
    test_lunar_core_flow();
    test_artifact_engine();
    test_extension_and_hooks();
    test_pipeline_engine();
    test_network_downloader();
    test_remote_repository_and_distribution();
    test_plugin_entries();
    std::cout << "All Lunar tests passed successfully (100%)!\n";
    return 0;
}
