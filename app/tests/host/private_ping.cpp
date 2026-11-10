/* Navi5 — private read-only authenticated ping, shared production transport.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/dev_login.h"
#include "subsonic/http_jf.h"
#include "jf/curl_diag.h"
#include <cstdio>
int main(int argc, char **argv) {
    std::string server, user, password;
    if (argc != 3 || !app::dev::load(argv[1], server, user, password)) {
        std::puts("config=invalid auth=pending"); return 2;
    }
    if (server.compare(0, 7, "http://") == 0 && std::string(argv[2]) != "--allow-http") {
        std::puts("consent=required auth=pending"); return 2;
    }
    subsonic::Client client(server, user, password, subsonic::http_get_jf);
    subsonic::ServerInfo info;
    std::string error;
    subsonic::LoginFailure failure{};
    const bool ok = client.ping(&info, &error, &failure);
    std::printf("ok=%d failure=%d %s\n", int(ok), int(failure), jf::curl_diag.line().c_str());
    return ok ? 0 : 1;
}
