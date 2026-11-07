#include <mcdk/world_project.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <mcdevtool/env.h>

namespace {
    namespace fs = std::filesystem;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    void writePack(const fs::path& directory, const char* uuid, const char* type) {
        fs::create_directories(directory);
        std::ofstream(directory / "manifest.json", std::ios::binary)
            << R"({"format_version":1,"header":{"name":"p","uuid":")" << uuid
            << R"(","version":[1,0,0]},"modules":[{"type":")" << type
            << R"(","uuid":"00000000-0000-4000-8000-0000000000ff","version":[1,0,0]}]})";
        std::ofstream(directory / "keep.txt", std::ios::binary) << "source";
    }

    bool isJunction(const fs::path& path) {
        return fs::symlink_status(path).type() == fs::file_type::junction;
    }

    void testLinksLandInsideTheWorld(const fs::path& root) {
        const auto source = root / fs::u8path("源码 包");
        const auto world  = root / fs::u8path("存档");
        writePack(source / "B", "11111111-0000-4000-8000-000000000001", "data");
        writePack(source / "R", "11111111-0000-4000-8000-000000000002", "resources");
        fs::create_directories(world);

        const auto roots = mcdk::prepareWorldPackRoots(world);
        const auto packs = MCDevTool::linkSourceAddonToRuntimePacks(source, roots);
        require(packs.size() == 2, "both packs are linked");
        mcdk::recordWorldPackLinks(world, packs);

        const auto behavior = world / "behavior_packs" / "11111111000040008000000000000001";
        const auto resource = world / "resource_packs" / "11111111000040008000000000000002";
        require(isJunction(behavior) && isJunction(resource), "packs are junctions inside the world");
        require(fs::is_regular_file(behavior / "keep.txt"), "junction reaches the source");
    }

    void testRelinkRemovesOnlyRecordedEntries(const fs::path& root) {
        const auto source = root / fs::u8path("源码 包");
        const auto world  = root / fs::u8path("存档");
        const auto stale  = world / "behavior_packs" / "11111111000040008000000000000001";

        // 地图自带的真实包目录，不在记录里。
        const auto mapPack = world / "behavior_packs" / "map_pack";
        writePack(mapPack, "22222222-0000-4000-8000-000000000001", "data");

        (void)mcdk::prepareWorldPackRoots(world);
        require(!fs::exists(stale), "recorded link is removed");
        require(fs::is_regular_file(source / "B" / "keep.txt"), "removing a link keeps the source");
        require(fs::is_regular_file(mapPack / "keep.txt"), "unrecorded pack is untouched");
    }

    void testRemovingLinksLeavesNothingToFollow(const fs::path& root) {
        const auto source = root / "removal_source";
        const auto world  = root / "removal_world";
        writePack(source, "33333333-0000-4000-8000-000000000001", "data");
        fs::create_directories(world);

        const auto packs = MCDevTool::linkSourceAddonToRuntimePacks(source, mcdk::prepareWorldPackRoots(world));
        mcdk::recordWorldPackLinks(world, packs);
        require(isJunction(world / "behavior_packs" / "33333333000040008000000000000001"), "link exists at runtime");

        mcdk::removeWorldPackLinks(world);
        require(!fs::exists(world / "behavior_packs" / "33333333000040008000000000000001"), "link is gone");
        require(!fs::exists(world / ".mcdk_pack_links.json"), "record is gone");
        require(fs::is_regular_file(source / "keep.txt"), "source survives");

        // 重复调用是 stop 和退出清理同时发生时的情况。
        mcdk::removeWorldPackLinks(world);
    }

    void testBrokenRecordsAreHarmless(const fs::path& root) {
        const auto world   = root / "broken";
        const auto outside = root / "outside.txt";
        fs::create_directories(world);
        std::ofstream(outside, std::ios::binary) << "x";

        std::ofstream(world / ".mcdk_pack_links.json", std::ios::binary) << R"(["../outside.txt", 1, ""])";
        (void)mcdk::prepareWorldPackRoots(world);
        require(fs::exists(outside), "entries cannot escape the world");

        std::ofstream(world / ".mcdk_pack_links.json", std::ios::binary) << "{ nope";
        (void)mcdk::prepareWorldPackRoots(world);
    }

    void testGlobalRootsAreUnchanged() {
        const auto roots = MCDevTool::getGlobalPackRoots();
        require(roots.behavior == MCDevTool::getBehaviorPacksPath(), "global behavior root");
        require(roots.resource == MCDevTool::getResourcePacksPath(), "global resource root");
    }
} // namespace

int main() {
    const auto root = fs::temp_directory_path() / fs::u8path("mcdk_存档链接_测试");
    try {
        fs::remove_all(root);
        fs::create_directories(root);
        testLinksLandInsideTheWorld(root);
        testRelinkRemovesOnlyRecordedEntries(root);
        testRemovingLinksLeavesNothingToFollow(root);
        testBrokenRecordsAreHarmless(root);
        testGlobalRootsAreUnchanged();
        fs::remove_all(root);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "world pack link tests passed\n";
    return EXIT_SUCCESS;
}
