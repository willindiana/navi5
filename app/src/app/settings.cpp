/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/settings.h"
#include "app/i18n.h"
#include "app/json_num.h"

#include <algorithm>

#include "evo_boot_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

extern "C" {
#include "cJSON.h"
}

namespace settings {
namespace {

constexpr const char *kFile = "/download0/navi5/settings.json";
std::mutex s_lock;
All s_all;
} // namespace

All get()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_all;
}

void load_local()
{
    std::string body;
    if (FILE *f = std::fopen(kFile, "rb")) {
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            body.append(buf, n);
        std::fclose(f);
    }
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return;
    std::lock_guard<std::mutex> g(s_lock);
    s_all.local.language = settings::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "language")));
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "nightMode"))
        s_all.local.night_mode = cJSON_IsTrue(v);
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "checkUpdates"))
        s_all.local.check_updates = cJSON_IsTrue(v);
    const int buffer = settings::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "bufferSeconds")));
    s_all.local.buffer_s = 30;   /* (missing or not a choice: the old fixed 30 s) */
    for (int c : kBufferChoices)
        if (c == buffer)
            s_all.local.buffer_s = c;
    if (const cJSON *hz = cJSON_GetObjectItemCaseSensitive(j, "refresh120"))
        s_all.local.refresh_120 = cJSON_IsTrue(hz);
    if (s_all.local.language < 0 || s_all.local.language >= i18n::ChoiceCount)
        s_all.local.language = 0;
    cJSON_Delete(j);
}

void set_local(const Local &l)
{
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_all.local = l;
    }
    mkdir("/download0/navi5", 0700);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "language", l.language);
    cJSON_AddBoolToObject(j, "refresh120", l.refresh_120);
    cJSON_AddNumberToObject(j, "bufferSeconds", l.buffer_s);
    cJSON_AddBoolToObject(j, "nightMode", l.night_mode);
    cJSON_AddBoolToObject(j, "checkUpdates", l.check_updates);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text)
        return;
    /* Beside and whole, then renamed over: a crash or a full disk mid-write keeps the
     * old settings instead of truncating them to defaults. (No fsync: this runs on
     * the render thread at every change, and a sync can take tens of ms.) */
    const std::string tmp = std::string(kFile) + ".tmp";
    const size_t n = std::strlen(text);
    bool ok = false;
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        ok = std::fwrite(text, 1, n, f) == n && std::fflush(f) == 0;
        ok = std::fclose(f) == 0 && ok;
    }
    if (!ok || std::rename(tmp.c_str(), kFile) != 0) {
        std::remove(tmp.c_str());
        evo_bt("settings: cannot write %s", kFile);
    }
    std::free(text);
}

} // namespace settings
