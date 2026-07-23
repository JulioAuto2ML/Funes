// =============================================================================
// tests/test_file_tools.cpp — fs_guard + read_file/write_file
// =============================================================================

#include "tools.h"
#include "tools/fs_guard.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_fs_guard() {
    fs::path ws = fs::temp_directory_path() / "funes_test_fsguard_ws";
    fs::remove_all(ws);
    fs::create_directories(ws / "sub");
    std::ofstream(ws / "sub" / "existing.txt") << "hi";

    // Inside the workspace, relative.
    auto ok = funes::fsguard::resolve(ws, "sub/existing.txt");
    CHECK(ok.has_value());
    CHECK(fs::weakly_canonical(*ok) == fs::weakly_canonical(ws / "sub" / "existing.txt"));

    // A path that doesn't exist yet is still fine (needed for write_file).
    auto new_file = funes::fsguard::resolve(ws, "brand/new/file.txt");
    CHECK(new_file.has_value());

    // Escapes via "..".
    CHECK(!funes::fsguard::resolve(ws, "../outside.txt").has_value());
    CHECK(!funes::fsguard::resolve(ws, "sub/../../outside.txt").has_value());
    CHECK(!funes::fsguard::resolve(ws, "../../../../../../etc/passwd").has_value());

    // Absolute path outside the workspace.
    CHECK(!funes::fsguard::resolve(ws, "/etc/passwd").has_value());

    // Absolute path that happens to be inside the workspace is fine.
    auto abs_inside = funes::fsguard::resolve(ws, (ws / "sub" / "existing.txt").string());
    CHECK(abs_inside.has_value());

    // Empty path is rejected.
    CHECK(!funes::fsguard::resolve(ws, "").has_value());

    fs::remove_all(ws);
    return 0;
}

int test_read_write_file() {
    fs::path ws = fs::temp_directory_path() / "funes_test_file_tools_ws";
    fs::remove_all(ws);

    ToolRegistry reg;
    register_file_tools(reg, ws.string());
    CHECK(reg.has("read_file") && reg.has("write_file"));

    ToolContext ctx{"funes", "s1"};

    // write_file creates parent dirs and the file.
    auto w1 = reg.call("write_file", {{"path", "notes/todo.txt"}, {"content", "buy milk"}}, ctx);
    CHECK(!w1.error);
    CHECK(fs::exists(ws / "notes" / "todo.txt"));

    // read_file reads it back.
    auto r1 = reg.call("read_file", {{"path", "notes/todo.txt"}}, ctx);
    CHECK(!r1.error);
    CHECK(r1.text == "buy milk");

    // append=true adds rather than replacing.
    auto w2 = reg.call("write_file", {{"path", "notes/todo.txt"}, {"content", "\nwalk dog"}, {"append", true}}, ctx);
    CHECK(!w2.error);
    auto r2 = reg.call("read_file", {{"path", "notes/todo.txt"}}, ctx);
    CHECK(r2.text == "buy milk\nwalk dog");

    // default (append=false) overwrites.
    auto w3 = reg.call("write_file", {{"path", "notes/todo.txt"}, {"content", "fresh start"}}, ctx);
    CHECK(!w3.error);
    auto r3 = reg.call("read_file", {{"path", "notes/todo.txt"}}, ctx);
    CHECK(r3.text == "fresh start");

    // Traversal is refused for both tools.
    auto w4 = reg.call("write_file", {{"path", "../escape.txt"}, {"content", "x"}}, ctx);
    CHECK(w4.error);
    auto r4 = reg.call("read_file", {{"path", "../../etc/passwd"}}, ctx);
    CHECK(r4.error);

    // Missing file.
    auto r5 = reg.call("read_file", {{"path", "does/not/exist.txt"}}, ctx);
    CHECK(r5.error);

    // Binary content is rejected by read_file, not dumped as garbage.
    {
        std::ofstream bin(ws / "image.bin", std::ios::binary);
        bin.write("\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01", 12);
    }
    auto r6 = reg.call("read_file", {{"path", "image.bin"}}, ctx);
    CHECK(r6.error);
    CHECK(r6.text.find("binary") != std::string::npos);

    // Content over the write cap is refused.
    auto w5 = reg.call("write_file", {{"path", "big.txt"}, {"content", std::string(300000, 'x')}}, ctx);
    CHECK(w5.error);

    fs::remove_all(ws);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_fs_guard();
    rc |= test_read_write_file();
    if (rc == 0) std::cout << "test_file_tools: all tests passed\n";
    return rc;
}
