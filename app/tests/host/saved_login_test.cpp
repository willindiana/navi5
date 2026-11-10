/* Navi5 — saved login storage checks; temp dir only, fake credentials.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/saved_login.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace app::saved_login;

static void put(const std::string &p, const std::string &body)
{
    FILE *f = std::fopen(p.c_str(), "wb");
    assert(f);
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
}

static mode_t mode(const std::string &p)
{
    struct stat st;
    assert(lstat(p.c_str(), &st) == 0);
    return st.st_mode & 0777;
}

int main()
{
    char tmpl[] = "/tmp/navi5-saved-login-XXXXXX";
    assert(mkdtemp(tmpl));
    const std::string dir = std::string(tmpl) + "/navi5", path = dir + "/login.json";
    Login out{"x", "x", "x"};

    assert(load(out, path.c_str()) == Load::Missing);
    assert(forget(path.c_str()));  // absent is fine

    const Login a{"http://192.0.2.1:4533", "alice", "p\"w\\\xc3\xa6"};
    assert(save(a, path.c_str()));
    assert(mode(dir) == 0700 && mode(path) == 0600);
    assert(access((path + ".tmp").c_str(), F_OK) != 0);
    assert(load(out, path.c_str()) == Load::Ok);
    assert(out.server == a.server && out.user == a.user && out.password == a.password);

    // Atomic replace: new content, same mode, stale temp/symlink temp tolerated.
    put(path + ".tmp", "junk");
    const Login b{"https://music.example.test/sub", "bob", ""};
    assert(save(b, path.c_str()));
    assert(mode(path) == 0600 && load(out, path.c_str()) == Load::Ok && out.user == "bob" && out.password.empty());
    assert(symlink("/nonexistent-target", (path + ".tmp").c_str()) == 0);
    assert(save(a, path.c_str()) && load(out, path.c_str()) == Load::Ok && out.user == "alice");

    // Invalid input never written.
    for (const char *u : {"ftp://h", "http://u:p@h", "http://h/?q", "http://h/#f", "http://", "h"})
        assert(!save({u, "alice", "pw"}, path.c_str()));
    assert(!save({"http://h", "", "pw"}, path.c_str()));
    assert(!save({"http://h", std::string(2000, 'u'), "pw"}, path.c_str()));
    assert(load(out, path.c_str()) == Load::Ok && out.user == "alice");

    // Rejected files leave out untouched.
    const Login keep = out;
    const char *bad[] = {
        "", "{", "[]", "null",
        R"({"server":"http://h","user":"u"})",
        R"({"server":"http://h","user":"u","password":1})",
        R"({"server":"http://h","user":"u","password":"p","token":"t"})",
        R"({"server":"http://u@h","user":"u","password":"p"})",
        R"({"server":"http://h?x","user":"u","password":"p"})",
        R"({"server":"gopher://h","user":"u","password":"p"})",
        R"({"server":"http://h","user":"","password":"p"})",
        R"({"server":"http://h","user":"u","password":"p"} x)",
        R"({"server":"http://h","user":"u","password":"p"}{})",
        R"({"server":"http://h","user":"u","password":"p\u0000tail"})",
        R"({"server":"http://h","user":"u\u0000","password":"p"})",
    };
    for (const char *body : bad) {
        put(path, body);
        assert(load(out, path.c_str()) == Load::Invalid);
        assert(out.user == keep.user && out.password == keep.password);
    }
    put(path, std::string(R"({"server":"http://h","user":"u","password":"ab)") + '\0' + "cd\"}");
    assert(load(out, path.c_str()) == Load::Invalid && out.password == keep.password);
    put(path, R"({"server":"http://h","user":"u","password":")" + std::string(9000, 'p') + "\"}");
    assert(load(out, path.c_str()) == Load::Invalid);
    // Exact bound: 8192 bytes (whitespace-padded) loads, 8193 rejected.
    const std::string ok_json = R"({"server":"http://h","user":"u","password":"p"})";
    put(path, ok_json + std::string(8192 - ok_json.size(), ' '));
    assert(load(out, path.c_str()) == Load::Ok && out.user == "u");
    put(path, ok_json + std::string(8193 - ok_json.size(), ' '));
    assert(load(out, path.c_str()) == Load::Invalid);
    // Escaped backslash before "u0000" is literal text, not NUL.
    put(path, R"({"server":"http://h","user":"u","password":"\\u0000"})");
    assert(load(out, path.c_str()) == Load::Ok && out.password == "\\u0000");
    unlink(path.c_str());
    assert(mkfifo(path.c_str(), 0600) == 0);
    assert(load(out, path.c_str()) == Load::Invalid);  // O_NONBLOCK: no hang on FIFO
    unlink(path.c_str());
    assert(symlink("/etc/hosts", path.c_str()) == 0);
    assert(load(out, path.c_str()) == Load::Invalid);  // O_NOFOLLOW
    unlink(path.c_str());

    assert(save(a, path.c_str()));
    put(path + ".tmp", "crash leftover");
    assert(forget(path.c_str()));
    assert(load(out, path.c_str()) == Load::Missing && access((path + ".tmp").c_str(), F_OK) != 0);

    unlink((path + ".tmp").c_str());
    rmdir(dir.c_str());
    rmdir(tmpl);
    std::puts("saved_login ok");
}
