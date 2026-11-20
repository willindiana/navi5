/*
 * Navi5 — music for PS5, forked from Jelly5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Hardware bring-up follows Nuvio PS5 and EVO Player.
 */
#include "app/i18n.h"
#include "app/smoke_session.h"
#include "app/spawn.h"
#include "app/pairing_flow.h"
#include "app/saved_login.h"
#include "subsonic/pairing_listener.h"
#include "ui/pairing_view.h"
#include "gfx/art.h"
#include "subsonic/http_jf.h"
#include "ui/music_screen.h"
#include "ui/login_view.h"
#include "ui/options_menu.h"
#include "ui/pad_route.h"
#include "ui/queue_view.h"
#include "ui/player_state.h"
#include <cmath>
#include <limits>
#include "app/settings.h"
#ifdef NAVI5_DEV_TOOLS
#include "app/dev_login.h"
#include "app/dev_reader_check.h"
#endif
#include "gfx/gfx.h"
#include "nuvio_input.h"
#include "platform/ime.h"
#include "platform/network_address.h"
#include "ui/screen.h"
#include "ui_text.h"
#include "evo_adec.h"
#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "evo_hw.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <time.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
int sceUserServiceInitialize(void *params);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int scePadInit(void);
int sceSystemServiceHideSplashScreen(void);
extern int g_ps5_user_id;
__attribute__((weak)) int nuvio_import_count(void) { return 0; }
__attribute__((weak)) const char *nuvio_import_null(int) { return nullptr; }
}

namespace {
/* The app's start, as the linker lays it out (for the crash report's offsets).
 * Its code is the first segment, about 24 MB: offsets under 32 MB are code. */
extern "C" char __ehdr_start[] __attribute__((weak));
constexpr uintptr_t kCodeSpan = 32u << 20;

void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    static volatile int s_in_crash = 0;
    if (s_in_crash++) {   /* a fault inside the report itself: let the first one through */
        signal(sig, SIG_DFL);
        return;
    }
    evo_bt("navi5: CRASH signal=%d addr=%p", sig, si ? si->si_addr : nullptr);
    /* Where: the faulting instruction and the code addresses on the stack, as
     * offsets into the app (llvm-symbolizer --obj=build/llvm-pie.elf 0x...). An
     * abort's own address is inside abort(); its callers are on the stack. Only
     * this thread's stack is read (its bounds from pthread), never past its top. */
    const uintptr_t lo = (uintptr_t)__ehdr_start, hi = lo + kCodeSpan;
    if (ctx && lo) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        const uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
        const uintptr_t rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
        uintptr_t top = 0;
        pthread_attr_t attr;
        void *base = nullptr;
        size_t size = 0;
        if (pthread_attr_init(&attr) == 0) {
            if (pthread_attr_get_np(pthread_self(), &attr) == 0 && pthread_attr_getstack(&attr, &base, &size) == 0)
                top = (uintptr_t)base + size;
            pthread_attr_destroy(&attr);
        }
        char line[512];
        int n = std::snprintf(line, sizeof line, "navi5: crash at %s%#lx; stack:", rip >= lo && rip < hi ? "+" : "",
                              (unsigned long)(rip >= lo && rip < hi ? rip - lo : rip));
        if (rsp && top > rsp) {
            const uintptr_t *sp = (const uintptr_t *)rsp;
            const size_t words = std::min<size_t>(1024, (top - rsp) / sizeof(uintptr_t));
            for (size_t i = 0, found = 0; i < words && found < 24 && n < (int)sizeof line - 16; i++)
                if (sp[i] >= lo && sp[i] < hi) {
                    n += std::snprintf(line + n, sizeof line - n, " +%#lx", (unsigned long)(sp[i] - lo));
                    found++;
                }
        } else {
            n += std::snprintf(line + n, sizeof line - n, " (bounds unknown)");
        }
        evo_bt("%s", line);
    }
    evo_boot_log_flush();
    signal(sig, SIG_DFL);   /* re-fault so the kernel writes its crash report */
}

void install_crash_handler()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}


double pairing_now()
{
    return std::chrono::duration<double>(pairing::Session::Clock::now().time_since_epoch()).count();
}

struct PairingWork {
    std::shared_ptr<pairing::Listener> listener = std::make_shared<pairing::Listener>();
    std::atomic<bool> checking{false}, finished{false};
    pairing::Diagnostic diagnostic;
    // Published by finished release/acquire; no render-thread network I/O.
    std::optional<pairing::Credentials> accepted;
};

struct SavedWork {
    enum Kind { Load, Save, Forget } kind = Load;
    std::atomic<bool> done{false};
    app::saved_login::Login login;
    app::saved_login::Load loaded = app::saved_login::Load::Invalid;
    bool ok = false;
};

struct Discovery {
    std::atomic<bool> done{false};
    std::vector<std::string> addresses; // Published by done release/acquire. Never log.
};

[[noreturn]] void wait_for_close()
{
    // Never exit from an app process. Close through PS button instead.
    for (;;) usleep(1000 * 1000);
}

} // namespace

int main()
{
    install_crash_handler();
    evo_bt("navi5: app start");
    for (int i = 0; i < nuvio_import_count(); i++)
        if (const char *name = nuvio_import_null(i))
            evo_bt("navi5: import %s is NULL on this console", name);

    evo_adec_native_probe();
    evo_hw_probe();
    if (evo_agc_runtime_init(1920, 1080, 0) != 0) {
        evo_bt("navi5: display init failed");
        wait_for_close();
    }
    evo_agc_runtime_frame_begin();
    evo_agc_runtime_present();
    sceSystemServiceHideSplashScreen();
    // FFmpeg diagnostics can contain credential-bearing URLs. Never emit them.
    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();

    sceUserServiceInitialize(nullptr);
    scePadInit();
    int users[4] = {0};
    if (sceUserServiceGetLoginUserIdList(users) != 0 || users[0] < 0) {
        evo_bt("navi5: no logged-in user");
        wait_for_close();
    }
    g_ps5_user_id = users[0];
    nuvio_input_open(users[0]);
    settings::load_local();
    i18n::set_choice(settings::get().local.language);
    if (ui_text_init() != 0 || !gfx::init()) {
        evo_bt("navi5: UI init failed");
        wait_for_close();
    }

    using namespace app::smoke;
    auto mailbox = std::make_shared<Mailbox>();
    const bool worker = start_worker(mailbox, subsonic::http_get_jf, {}, subsonic::http_send_jf);
    uint64_t generation = mailbox->reset();
    PlaybackSnapshot playback;
    uint64_t history_seen = 0;
    auto reset_session = [&] {
        generation = mailbox->reset();
        playback.invalidate(generation);
    };
    std::string server, username, password, note, diagnostic;
#ifdef NAVI5_DEV_TOOLS
    // Startup only, before render loop. Missing/invalid private input leaves fields empty.
    // Prefill only: no auto-connect; HTTP consent still required by login().
    bool cleanup_failed = false;
    const bool imported = app::dev::import_temporary(server, username, password, cleanup_failed);
    const auto temporary_status = app::dev::import_status;
    if (!imported) app::dev::load("/download0/navi5/dev-login.json", server, username, password);
    const auto private_status = imported ? app::dev::ImportStatus{} : app::dev::import_status;
    auto reader_check = std::make_shared<app::dev::ReaderCheck>();
    if (!jelly5::spawn([reader_check] {
            app::dev::check_reader(*reader_check, "/app0/.navi5-reader-fixture.tmp");
        })) reader_check->stage.store(11);
    unsigned delivered = 0;
#endif
    auto saved_work = std::make_shared<SavedWork>();
    if (!jelly5::spawn([work = saved_work] {
            work->loaded = app::saved_login::load(work->login);
            work->done.store(true, std::memory_order_release);
        })) saved_work->done.store(true, std::memory_order_release);
    bool saved_loaded = false, saved_connect = false, saved_retry = false;
    bool saved_key = false; // Saved file holds a server-side key: Forget explains revocation.
    app::saved_login::Login pending_login;
    std::string saved_api_key; // Loaded key for saved connect/retry only. Never shown or logged.
    bool connected = false, connecting = false, consent = false, warning = false, nav = false;
    bool scheme_popup = false, choose_http = false;
    // Login screen: entry form left, QR area right; whichever completes first wins.
    // Pairing opens whenever the screen appears (qr_wanted) and stops when it is left.
    ui::LoginView login_view;
    ui::SavedFailedView saved_failed;
    bool login_screen_was = false, qr_wanted = false, qr_needed_was = false;
    bool show_password = false;
    ui::OptionsMenu menu;
    ui::QueueView queue_view;
    bool logout_forget = false; // Logout during an unfinished Save: forget right after it.
    uint64_t enqueue_serial = 0;
    std::string toast; // Short confirmation, e.g. Play Next. Fixed local text plus titles only.
    double toast_until = 0;
    ui::MusicScreen music;
    ui::PairingView pairing_view;
    std::shared_ptr<PairingWork> pairing_work;
    bool pairing_open = false;
    app::PairingFlow pairing_flow;
    std::string pairing_address; // Current session's bind address, for restarts. Never log.
    auto discovery = std::make_shared<Discovery>();
    bool discovery_seen = false;
    if (!jelly5::spawn([discovery] {
            discovery->addresses = netaddr::private_ipv4();
            discovery->done.store(true, std::memory_order_release);
        })) discovery->done.store(true, std::memory_order_release); // Empty: manual paths.
    // Open sheet from discovery: one address starts QR at once, several show picker, none IME.
    auto open_pairing = [&] {
        pairing_flow.restarts = 2; pairing_address.clear();
        static const std::vector<std::string> none;
        const auto &found = discovery_seen ? discovery->addresses : none; // Read only after acquire.
        if (found.size() == 1) pairing_view.start();
        else {
            pairing_view.reset();
            // ponytail: picker fits 3 rows above button; more falls back to IME entry.
            if (found.size() > 1 && found.size() <= 3) pairing_view.choose(found);
        }
        pairing_open = true;
    };
    auto start_listener = [&](const std::string &address) {
        pairing_address = address;
        auto work = std::make_shared<PairingWork>();
        pairing_work = work;
        if (!jelly5::spawn_media([work, address] {
            work->listener->run([work](const pairing::Credentials &credentials, subsonic::LoginFailure &failure) {
                work->checking.store(true);
                const bool ok = pairing::validate(credentials, subsonic::http_get_jf, &failure);
                work->checking.store(false);
                return ok;
            }, address, true);
            work->diagnostic = work->listener->diagnostic();
            work->accepted = work->listener->take();
            work->finished.store(true, std::memory_order_release);
        })) {
            work->diagnostic.record(pairing::Stage::WorkerSpawn);
            work->listener->cancel();
            work->finished.store(true, std::memory_order_release);
        }
    };
    ui::PlayerState player_view;
    ui::Anim controls_alpha;
    ui::Drop transport_drop;
    ui::Drop login_drop, menu_drop;
    double previous = art::now();
    uint64_t play_serial = 0, play_wanted = 0;
    auto send = [&](Command c) { c.generation = generation; return worker && mailbox->send(std::move(c)); };
    auto submit_login = [&](Command c) {
        // Exactly one secret. Result may replace a password with an issued key before saving.
        pending_login = !c.api_key.empty() ? app::saved_login::Login{c.server, c.username, "", c.api_key}
            : !c.password.empty() ? app::saved_login::Login{c.server, c.username, c.password, ""}
            : app::saved_login::Login{};
        return send(std::move(c));
    };
    auto saved_command = [&] {
        Command c; c.kind = Command::Login; c.server = server; c.username = username;
        if (saved_api_key.empty()) c.password = password; else c.api_key = saved_api_key;
        return c;
    };
    auto persist = [&](SavedWork::Kind kind, app::saved_login::Login value = {}) {
        auto work = std::make_shared<SavedWork>();
        work->kind = kind;
        saved_work = work; // Only one disk operation at a time; Forget waits for Save.
        if (!jelly5::spawn([work, value = std::move(value)] {
                work->ok = work->kind == SavedWork::Save ? app::saved_login::save(value) : app::saved_login::forget();
                work->done.store(true, std::memory_order_release);
            })) work->done.store(true, std::memory_order_release);
    };
    // Same generation/logout path as every account switch: stale results are
    // dropped, the worker stops playback and forgets its client. The saved login
    // is deleted through the Forget path; nothing can revoke the key from here.
    auto logout = [&] {
        reset_session(); connected = false; nav = false; consent = false;
        server.clear(); username.clear(); password.clear(); saved_api_key.clear(); pending_login = {};
        saved_connect = saved_retry = false; note.clear(); diagnostic.clear(); toast.clear();
        music = ui::MusicScreen{}; player_view.dismiss(); queue_view.close(); menu.close(); play_wanted = 0;
        login_view = {}; show_password = false;
        if (saved_work) logout_forget = true; else persist(SavedWork::Forget);
    };
    auto login = [&] {
        show_password = false;
        if (saved_work) { note = T("Vent til lagring er ferdig."); return; }
        saved_connect = saved_retry = false; saved_api_key.clear();
        if (!worker) { note = T("Arbeidstråd kunne ikke starte. Start appen på nytt."); return; }
        const auto input = prepare_server(server);
        if (input == ServerInput::NeedsScheme) { scheme_popup = true; choose_http = false; return; }
        if (input == ServerInput::Invalid) { note = T("Ugyldig serveradresse. Bruk http:// eller https:// uten kontodata."); return; }
        if (username.empty()) { note = T("Skriv inn brukernavn."); return; }
        if (password.empty()) { note = T("Skriv inn passord."); return; }
        if (server.compare(0, 7, "http://") == 0 && !consent) { warning = true; return; }
        Command c; c.kind = Command::Login; c.server = server; c.username = username; c.password = password;
        // Failed manual connects keep password in RAM for explicit retry. Never log.
        connecting = submit_login(std::move(c));
        note = connecting ? T("Kobler til …") : T("Kunne ikke starte arbeidet");
    };
    for (;;) {
        nuvio_input_state in;
        nuvio_input_poll(&in);
        const bool keyboard = ime::active();
        ime::poll();
        std::deque<Result> results;
#ifdef NAVI5_DEV_TOOLS
        const bool mailbox_busy = !playback.poll(*mailbox, generation, results, true);
#else
        playback.poll(*mailbox, generation, results, false);
#endif
        const auto &audio = playback.audio;
        const auto &song_id = playback.song_id, &track = playback.title, &artist = playback.artist;
        const auto &artwork = playback.artwork_url, &portrait = playback.portrait_url, &next_title = playback.next_title;
        const auto &duration = playback.duration;
        const auto &audio_generation = playback.generation, &playback_revision = playback.revision;
        const auto &shuffle_enabled = playback.shuffle_enabled, &shuffle_pending = playback.shuffle_pending;
        if (saved_work && saved_work->done.load(std::memory_order_acquire)) {
            auto work = std::move(saved_work);
            if (work->kind == SavedWork::Load) {
                saved_loaded = true;
                if (work->loaded == app::saved_login::Load::Ok) {
                    server = std::move(work->login.server); username = std::move(work->login.user);
                    password = std::move(work->login.password); saved_api_key = std::move(work->login.api_key);
                    saved_key = !saved_api_key.empty();
                    consent = true; saved_connect = true;
                    connecting = submit_login(saved_command());
                    saved_retry = !connecting;
                    note = connecting ? T("Kobler til med lagret innlogging …") : T("Kunne ikke starte arbeidet");
                }
            } else if (work->kind == SavedWork::Save) {
                if (!work->ok) note = T("Kunne ikke lagre innlogging. Økten fortsetter.");
                if (logout_forget) { logout_forget = false; note.clear(); persist(SavedWork::Forget); }
            } else {
                password.clear(); saved_api_key.clear(); pending_login = {}; saved_connect = saved_retry = consent = false;
                // Forgetting cannot revoke: that needs the password this file no longer holds.
                note = !work->ok ? T("Kunne ikke glemme lagret innlogging.")
                    : saved_key ? T("Lagret innlogging glemt. Slett spilleren «Navi5 (PS5)» i Navidrome for å tilbakekalle nøkkelen.")
                    : T("Lagret innlogging glemt.");
                if (work->ok) saved_key = false;
            }
        }
        if (connected && history_seen != playback.history_revision) {
            history_seen = playback.history_revision;
            music.state().history_changed();
        }
        for (auto &result : results) {
            if (result.generation != generation) continue;
            if (result.kind == Command::Login) {
#ifdef NAVI5_DEV_TOOLS
                ++delivered;
#endif
                connecting = false; connected = result.ok;
                note = result.ok ? "" : T(login_failure_key(result.failure));
                diagnostic = result.diagnostic.line();
                if (connected) {
                    if (!result.api_key.empty()) {
                        // Key in use (supplied or just issued): it replaces any password on disk.
                        pending_login.password.clear(); pending_login.api_key = std::move(result.api_key);
                        if (!result.username.empty()) pending_login.user = username = std::move(result.username);
                    }
                    if (!pending_login.password.empty() || !pending_login.api_key.empty()) {
                        saved_key = !pending_login.api_key.empty();
                        persist(SavedWork::Save, std::move(pending_login));
                    }
                    pending_login = {}; saved_api_key.clear(); saved_connect = saved_retry = false;
                    history_seen = playback.history_revision;
                    password.clear(); music = ui::MusicScreen{}; music.activate(); nav = false;
                } else if (saved_connect) {
                    // Network and credential failures alike: the file is kept and the
                    // failed-login screen offers Retry, Forget, or signing in again.
                    saved_retry = true; saved_failed = {};
                }
            } else if (result.kind == Command::Browse && connected) {
                music.state().complete(result.token, std::move(result.page), result.ok);
            } else if (result.kind == Command::Play && connected) {
                // Only newest unacknowledged intent may present; Circle/Stop/logout clear it.
                if (result.token != play_wanted) continue;
                play_wanted = 0;
                if (result.ok) { player_view.open(art::now()); note.clear(); }
                else note = result.playback_error.empty() ? T("Kunne ikke starte avspilling") : T(result.playback_error.c_str());
            } else if (result.kind == Command::Enqueue && connected) {
                if (result.ok) {
                    toast = std::string(T("Spilles neste:")) + " " + (result.count == 1 && !result.title.empty()
                        ? result.title : TN(int(result.count), "%d spor", "%d spor"));
                } else toast = result.playback_error.empty() ? T("Kunne ikke laste spor til køen") : T(result.playback_error.c_str());
                toast_until = art::now() + 4;
            }
        }
        if (!discovery_seen && discovery->done.load(std::memory_order_acquire)) discovery_seen = true;
        const bool qr_possible = discovery_seen && !discovery->addresses.empty();
        const bool login_screen = !connected && !connecting && !saved_retry && saved_loaded && !saved_work;
        if (login_screen && !login_screen_was) qr_wanted = true;
        login_screen_was = login_screen;
        if (!login_screen) show_password = false;
        if (!login_screen && pairing_open) {
            // Leaving the login screen (manual connect in flight): stop the listener.
            if (pairing_work) pairing_work->listener->cancel();
            pairing_view.input(NUVIO_BTN_CIRCLE, pairing_now());
            pairing_open = false;
        }
        // QR first: one address starts the session at once, two or three show the
        // picker, more ask for the address; none leaves manual entry only.
        if (qr_wanted && login_screen && discovery_seen && !pairing_open && !pairing_work && !keyboard && !ime::active()) {
            qr_wanted = false;
            if (qr_possible) {
                open_pairing();
                if (pairing_view.state() == ui::PairingView::State::Starting) {
                    if (pairing_flow.begin(pairing_view, false, pairing_now(), false)) start_listener(discovery->addresses[0]);
                } else login_view.attention(true);
            }
        }
        if (!pairing_open && pairing_work && pairing_work->finished.load(std::memory_order_acquire))
            pairing_work.reset(); // Drop cancelled/stale credentials even if sheet stays closed.
        const bool seek_active = audio_generation == generation &&
            (audio.state == app::AudioState::playing || audio.state == app::AudioState::paused);
        player_view.sync(playback_revision, audio.track, seek_active);
        const bool can_seek = ui::PlayerState::seekable(audio.duration, audio.position, seek_active,
            audio.seeking || audio.track != audio.next_track);
        uint32_t pressed = keyboard || ime::active() || (!connected && saved_work) ? 0 : in.pressed;
        if (!connected) {
            uint32_t qr_pressed = 0;
            if (saved_retry) {
                using Failed = ui::SavedFailedView::Action;
                const auto action = saved_failed.input(pressed);
                if (action == Failed::Login) {
                    saved_retry = saved_connect = false;
                    password.clear(); saved_api_key.clear(); pending_login = {};
                } else if (action == Failed::Retry) {
                    saved_retry = false;
                    connecting = submit_login(saved_command());
                    saved_retry = !connecting;
                    note = connecting ? T("Kobler til med lagret innlogging …") : T("Kunne ikke starte arbeidet");
                } else if (action == Failed::Forget) {
                    saved_retry = saved_connect = false;
                    persist(SavedWork::Forget);
                }
            } else if (scheme_popup) {
                if (pressed & NUVIO_BTN_CIRCLE) scheme_popup = false;
                else if (pressed & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) choose_http = !choose_http;
                else if (pressed & NUVIO_BTN_CROSS) {
                    server = std::string(choose_http ? "http://" : "https://") + server;
                    scheme_popup = false; consent = false;
                    if (choose_http) warning = true;
                }
            } else if (warning) {
                if (pressed & NUVIO_BTN_CIRCLE) warning = false;
                else if (pressed & NUVIO_BTN_CROSS) { consent = true; warning = false; login(); }
            } else if (connecting) {
                // Circle only drops the stale result (generation). It cannot cancel a worker
                // blocked inside a syscall: that worker stays busy until the syscall returns.
                if (pressed & NUVIO_BTN_CIRCLE) {
                    reset_session(); connecting = false; saved_connect = saved_retry = false;
                    password.clear(); saved_api_key.clear(); pending_login = {}; note.clear();
                }
            } else if (login_screen) {
                using Login = ui::LoginView::Action;
                const bool on_form = login_view.focus != ui::LoginView::Focus::Qr;
                if (on_form && (pressed & NUVIO_BTN_CIRCLE)) show_password = false;
                const auto action = login_view.input(pressed, qr_possible, qr_pressed);
                if (action == Login::Server) {
                    ime::request(ime::Kind::Url, T("Serveradresse"), server, [&](const std::string &value) {
                        server = value; consent = false; show_password = false; note.clear();
                        const auto input = prepare_server(server);
                        scheme_popup = input == ServerInput::NeedsScheme; choose_http = false;
                        if (input == ServerInput::Invalid) note = T("Ugyldig serveradresse. Bruk http:// eller https:// uten kontodata.");
                    });
                } else if (action == Login::Username) {
                    ime::request(ime::Kind::Text, T("Brukernavn"), username, [&](const std::string &value) { username = value; });
                } else if (action == Login::Password) {
                    show_password = false;
                    ime::request(ime::Kind::Password, T("Passord"), "", [&](const std::string &value) { password = value; });
                } else if (action == Login::ToggleShow) show_password = !show_password;
                else if (action == Login::Connect) login();
                // Stopped with Circle earlier: Cross on the QR area starts a fresh session.
                if (!pairing_open && qr_possible && (qr_pressed & NUVIO_BTN_CROSS)) {
                    qr_pressed = 0;
                    if (!pairing_work || pairing_work->finished.load(std::memory_order_acquire)) {
                        pairing_work.reset(); open_pairing();
                        if (pairing_view.state() == ui::PairingView::State::Starting &&
                                pairing_flow.begin(pairing_view, false, pairing_now(), false))
                            start_listener(discovery->addresses[0]);
                    } else note = T("Vent til paring er stoppet.");
                }
            }
            if (pairing_open) {
                using View = ui::PairingView;
                if (pairing_view.state() == View::State::Failed) pairing_address.clear();
                const auto action = pairing_view.input(qr_pressed, pairing_now());
                if (action == View::Action::Cancel) {
                    if (pairing_work) pairing_work->listener->cancel();
                    if (pairing_view.state() == View::State::Closed) pairing_open = false;
                } else if (action == View::Action::Start || action == View::Action::Retry) {
                    // Start: picker choice, else IME. Retry: same address as before, else IME.
                    const std::string address = action == View::Action::Start ? pairing_view.address() : pairing_address;
                    if (pairing_flow.begin(pairing_view,
                            pairing_work && !pairing_work->finished.load(std::memory_order_acquire), pairing_now(),
                            address.empty())) {
                        pairing_work.reset();
                        if (!address.empty()) start_listener(address);
                        // Explicit exact local IPv4 when discovery found nothing usable.
                        else ime::request(ime::Kind::Text, T("Konsollens lokale IPv4-adresse"), "",
                            [&](const std::string &typed) {
                                pairing_flow.address_pending = false;
                                if (!pairing_open || pairing_view.generation() != pairing_flow.generation) return;
                                start_listener(typed);
                            });
                    }
                }
                pairing_flow.address_closed(pairing_view, ime::active(), pairing_now(), ime::cancelled());
                if (pairing_view.state() == View::State::Closed) pairing_open = false;
                if (pairing_work && pairing_open) {
                    const bool finished = pairing_work->finished.load(std::memory_order_acquire);
                    // Read credentials only after worker's release publication.
                    std::optional<pairing::Credentials> accepted;
                    if (finished) accepted = std::move(pairing_work->accepted);
                    auto handoff = pairing_flow.update(pairing_view, pairing_work->listener->ready(),
                        pairing_work->checking.load(), finished, accepted, pairing_now(),
                        [&](pairing::Credentials credentials) {
                            Command c; c.kind = Command::Login;
                            c.server = std::move(credentials.server); c.username = std::move(credentials.username);
                            c.password = std::move(credentials.password); c.api_key = std::move(credentials.api_key);
                            reset_session();
                            password.clear(); saved_api_key.clear(); show_password = false;
                            server = c.server; username = c.username; saved_connect = saved_retry = false;
                            return submit_login(std::move(c));
                        });
                    if (finished) {
                        pairing_view.diagnose(pairing_flow.generation, pairing_work->diagnostic);
                        pairing_work.reset();
                    }
                    if (handoff) {
                        connecting = *handoff; pairing_open = false;
                        note = connecting ? T("Kobler til …") : T("Kunne ikke starte arbeidet");
                    }
                }
                if (pairing_view.state() == View::State::Failed) pairing_address.clear();
                // Expiry with drained worker: fresh nonce/QR until budget ends, then wait for Cross.
                if (pairing_open && !pairing_work && !pairing_address.empty() && pairing_flow.auto_restart(pairing_view, false) &&
                        pairing_flow.begin(pairing_view, false, pairing_now(), false))
                    start_listener(pairing_address);
                // Retry needs Cross on the QR area: bring focus there once.
                const bool needs_pad = pairing_open && (pairing_view.state() == View::State::Failed ||
                    (pairing_view.state() == View::State::Expired && pairing_flow.restarts <= 0));
                if (needs_pad && !qr_needed_was && !scheme_popup && !warning) login_view.attention(true);
                qr_needed_was = needs_pad;
            } else qr_needed_was = false;
        } else {
            music.session(audio_generation == generation && !song_id.empty() && (audio.state == app::AudioState::playing || audio.state == app::AudioState::paused),
                          audio.state == app::AudioState::paused, track, artist, artwork);
            if (queue_view.visible() && (!player_view.visible || !seek_active)) queue_view.close();
            // One table for the global buttons (ui/pad_route.h); the rest goes to the focused view.
            const ui_pad_context pad{player_view.visible, queue_view.visible(), menu.open(), music.searching(), !song_id.empty()};
            const ui_pad_route route = ui_route_pad(pressed, pad);
            pressed = route.rest;
            if (route.menu) menu.show();
            else if (menu.open()) {
                if (menu.input(pressed) == ui::OptionsMenu::Action::Logout) logout();
                pressed = 0;
            }
            if (connected) {
                if (route.search_backspace) music.search_backspace(false);
                if (route.pause_toggle) {
                    Command c; c.kind = audio.state == app::AudioState::paused ? Command::Resume : Command::Pause;
                    if (!send(std::move(c))) note = T("Kunne ikke starte arbeidet");
                }
                if (route.open_player) player_view.open(art::now());
                if (route.play_next) {
                    if (auto target = music.play_next_target()) {
                        const bool single = target->kind == ui::music::Kind::Track;
                        Command c; c.kind = Command::Enqueue; c.item = std::move(*target);
                        c.request.token = ++enqueue_serial;
                        toast = send(std::move(c)) ? (single ? "" : T("Henter spor …")) : T("Kunne ikke starte arbeidet");
                        toast_until = art::now() + 4;
                    }
                }
                if (queue_view.visible()) {
                    const auto action = queue_view.input(pressed, art::now());
                    if (action.kind == ui::QueueView::Action::Move || action.kind == ui::QueueView::Action::Remove) {
                        Command c; c.kind = action.kind == ui::QueueView::Action::Move ? Command::QueueMove : Command::QueueRemove;
                        c.uid = action.uid; c.anchor = action.anchor;
                        if (!send(std::move(c))) { toast = T("Kunne ikke starte arbeidet"); toast_until = art::now() + 4; }
                    }
                    player_view.activity = art::now(); // Controls stay awake under the view.
                } else
                if (player_view.visible) {
                    const double time = art::now();
                    if (pressed & NUVIO_BTN_CIRCLE) {
                        if (player_view.preview) { player_view.preview = false; player_view.activity = time; }
                        else { player_view.dismiss(); play_wanted = 0; }
                    } else if (pressed & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT | NUVIO_BTN_UP | NUVIO_BTN_DOWN)) {
                        player_view.navigate(pressed & NUVIO_BTN_LEFT ? -1 : 1,
                            pressed & NUVIO_BTN_DOWN ? 1 : pressed & NUVIO_BTN_UP ? -1 : 0,
                            time, can_seek, audio.position, audio.duration);
                    } else if (pressed & NUVIO_BTN_CROSS) {
                        if (player_view.controls(time) && player_view.focus == 3) {
                            if (can_seek && player_view.preview) {
                                Command c; c.kind = Command::Seek; c.seconds = player_view.target;
                                c.track = audio.track; c.playback_revision = playback_revision;
                                player_view.seek_error = !send(std::move(c)); player_view.preview = false;
                            }
                        } else if (player_view.focus == 4) {
                            if (seek_active && player_view.toggle_shuffle(time, shuffle_pending)) {
                                Command c; c.kind = Command::ShuffleToggle; c.shuffle_enabled = !shuffle_enabled;
                                c.playback_revision = playback_revision;
                                if (!send(std::move(c))) note = T("Kunne ikke starte arbeidet");
                            }
                        } else if (player_view.controls(time) && player_view.focus == 5) {
                            play_wanted = 0;
                            Command c; c.kind = Command::Stop;
                            if (!send(std::move(c))) note = T("Kunne ikke starte arbeidet");
                            player_view.dismiss();
                        } else if (player_view.controls(time) && player_view.focus == 6) {
                            if (seek_active) queue_view.open();
                        } else if (player_view.controls(time) && player_view.focus <= 2) {
                            Command c; c.kind = player_view.focus == 0 ? Command::Previous : player_view.focus == 2 ? Command::Next : audio.state == app::AudioState::paused ? Command::Resume : Command::Pause;
                            c.playback_revision = playback_revision;
                            if (!send(std::move(c))) note = T("Kunne ikke starte arbeidet");
                        }
                        player_view.activity = time;
                    } else if (pressed) player_view.activity = time;
                } else if (nav) {
                    if (pressed & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE)) { nav = false; music.enter_from_top(); }
                } else if (music.input(pressed).kind == ui::Action::ToNav) nav = true;
                if (music.take_menu_open()) menu.show(); // Header gear; Options is the shortcut.
                if (music.take_player_open() && audio_generation == generation && !song_id.empty() &&
                        (audio.state == app::AudioState::playing || audio.state == app::AudioState::paused)) {
                    if (audio.state == app::AudioState::paused) {
                        Command c; c.kind = Command::Resume;
                        if (!send(std::move(c))) note = T("Kunne ikke starte arbeidet");
                    }
                    player_view.open(art::now());
                }
                music.set_focused(!nav);
            }
        }
        if (connected) {
            mailbox->watch_queue(queue_view.visible(), queue_view.wanted_first(), generation);
            queue_view.sync(playback.queue, art::now());
            music.state().tick(art::now());
            if (auto request = music.state().take_request()) {
                ui::music::Page page;
                if (mailbox->cached(*request, page, generation))
                    music.state().complete(request->token, std::move(page));
                else {
                    Command c; c.kind = Command::Browse; c.request = *request;
                    if (!send(std::move(c))) music.state().complete(request->token, {}, false);
                }
            }
            if (auto play = music.state().take_play_intent()) {
                Command c; c.kind = Command::Play; c.play = std::move(*play);
                c.request.token = ++play_serial;
                play_wanted = 0;
                if (send(std::move(c))) play_wanted = play_serial;
                else note = T("Kunne ikke starte arbeidet");
            }
        }
        const double now = art::now();
        const float dt = static_cast<float>(std::min(0.05, now - previous)); previous = now;
        gfx::begin_frame();
        art::tick();
        if (const gfx::Texture *bg = ui::launch_backdrop()) gfx::image({0, 0, gfx::W, gfx::H}, bg);
        if (connected) {
            if (!player_view.visible) music.draw(now, dt);
            const char *status = T("Stoppet");
            switch (audio.state) {
            case app::AudioState::opening: status = T("Åpner lyd …"); break;
            case app::AudioState::playing: status = T("Spiller"); break;
            case app::AudioState::paused: status = T("Pause"); break;
            case app::AudioState::ended: status = T("Ferdig"); break;
            case app::AudioState::error: status = T("Lydfeil. Kontroller nettverk, CA-fil og format."); break;
            default: break;
            }
#ifdef NAVI5_DEV_TOOLS
            char audio_error[96];
            if (audio.state == app::AudioState::error) {
                std::snprintf(audio_error, sizeof audio_error, "DEV audio=%d curl=%d HTTP=%d multi=%d/%d",
                              int(audio.error_code), audio.curl_result, audio.http_status,
                              int(audio.multi_op), audio.multi_result);
                status = audio_error;
            }
#endif
            if (seek_active) {
                if (audio.seeking || audio.buffering) status = T("Søker / bufrer …");
                else if (audio.seek_failed || player_view.seek_error) status = T("Kunne ikke søke. Prøv igjen.");
            }
            const int elapsed = std::isfinite(audio.position) && audio.position >= 0 &&
                audio.position <= std::numeric_limits<int>::max() ? static_cast<int>(audio.position) : 0;
            char position[48]; std::snprintf(position, sizeof position, "  %d:%02d", elapsed / 60, elapsed % 60);
            if (player_view.visible) {
                controls_alpha.to(player_view.controls(now) ? 1.f : 0.f); controls_alpha.step(dt);
                const ui::PlaybackPresentation presentation{track, artist, artwork, portrait, status, next_title,
                    audio.position, audio.duration > 0 ? audio.duration : static_cast<double>(duration), audio.state == app::AudioState::paused,
                    audio.state == app::AudioState::playing, audio.state == app::AudioState::error,
                    can_seek, audio.seeking || audio.buffering, audio.seek_failed || player_view.seek_error,
                    player_view.preview, player_view.target, shuffle_enabled, shuffle_pending, playback.shuffle_error};
                ui::draw_music_player(presentation, player_view.focus, controls_alpha.value, dt, transport_drop);
                queue_view.draw(dt);
            } else if (music.active_session()) {
                ui::glass_panel({140, 950, 1640, 100}, 24);
                art::draw({158, 965, 70, 70}, artwork, "", 720, 720, 8);
                gfx::text(250, 992, track.empty() ? T("Uten tittel") : track, {gfx::SemiBold, 25, 760}, ui::kText);
                gfx::text(250, 1026, artist, {gfx::Medium, 22, 760}, ui::kText2);
                gfx::text(1750, 990, std::string(status) + position, {gfx::Medium, 22, 700}, ui::kText2, 2);
                ui::draw_pad_hints(1750, 1018, {{ui::PadButton::Touchpad, T("Fullskjermspiller")}}, 2, 26);
                ui::draw_mini_player_progress(audio.position, audio.duration);
            }
            if (!toast.empty() && now < toast_until)
                gfx::text(1820, 100, toast, {gfx::Medium, 24, 760}, ui::kText, 2);
            if (menu.state == ui::OptionsMenu::State::Menu) {
                // Drops down under the header gear (x 1744..1820).
                ui::glass_panel({1400, 146, 420, 104}, 28);
                bool moving = false;
                menu_drop.to({1414, 160, 392, 76}, 0); menu_drop.draw(dt, 1, &moving, 20);
                gfx::text(1446, 210, T("Logg ut"), {gfx::SemiBold, 30, 330}, ui::kText);
            } else if (menu.state == ui::OptionsMenu::State::Confirm) {
                gfx::fill({0, 0, gfx::W, gfx::H}, 0xa0000000);
                ui::glass_panel({410, 290, 1100, 500}, 40);
                gfx::text(960, 380, T("Logge ut?"), {gfx::Bold, 40, 1000}, ui::kText, 1);
                // The key outlives logout: revoking needs the password, which is no longer held.
                gfx::text_centered(960, 450, saved_key
                    ? T("Avspillingen stopper og lagret innlogging slettes fra konsollen. API-nøkkelen er fortsatt gyldig på serveren til du sletter spilleren «Navi5 (PS5)» i Navidrome. Navi5 har ikke lenger passordet og kan ikke tilbakekalle den.")
                    : T("Avspillingen stopper og lagret innlogging slettes fra konsollen."),
                    {gfx::Medium, 26, 980, 6}, ui::kText);
                ui::draw_pad_hints(960, 730, {{ui::PadButton::Cross, T("Logg ut")}, {ui::PadButton::Circle, T("Avbryt")}}, 1, 28);
            }
        } else {
            ui::glass_panel({160, 140, 1600, 790}, 48);
            gfx::text(960, 215, T("Navi5"), {gfx::Bold, 52}, ui::kText, 1);
            bool moving = false;
            if (saved_retry) {
                gfx::text(960, 420, T("Lagret innlogging mislyktes. Filen beholdes."), {gfx::Bold, 30, 1080}, ui::kText, 1);
                login_drop.to({610, 498.f + saved_failed.row * 76, 700, 64}, 10 + saved_failed.row);
                login_drop.draw(dt, 1, &moving, 22);
                gfx::text(960, 540, T("Prøv igjen"), {gfx::SemiBold, 30, 640}, ui::kText, 1);
                gfx::text(960, 616, T("Glem lagret innlogging"), {gfx::SemiBold, 30, 640}, ui::kText, 1);
                ui::draw_pad_hints(960, 740, {{ui::PadButton::Cross, T("Velg")}, {ui::PadButton::Circle, T("Logg inn på nytt")}}, 1, 28);
            } else if (scheme_popup) {
                gfx::text(960, 410, T("Velg protokoll for serveradressen"), {gfx::Bold, 30, 1080}, ui::kText, 1);
                gfx::text(960, 500, choose_http ? "HTTPS" : "> HTTPS", {gfx::Medium, 30}, ui::kText, 1);
                gfx::text(960, 560, choose_http ? "> HTTP" : "HTTP", {gfx::Medium, 30}, ui::kText, 1);
                gfx::text(960, 680, T("Opp/ned: velg   Cross: bekreft   Circle: avbryt"), {gfx::Medium, 27}, ui::kText2, 1);
            } else if (warning) {
                gfx::text(960, 410, T("HTTP sender innlogging og musikk ukryptert."), {gfx::Bold, 30, 1080}, ui::kText, 1);
                gfx::text(960, 470, T("Bruk kun et betrodd nettverk. Andre kan stjele kontoen."), {gfx::Medium, 27, 1080}, ui::kText, 1);
                gfx::text(960, 620, T("Cross: godta og koble til   Circle: avbryt"), {gfx::Medium, 28}, ui::kText, 1);
            } else if (connecting) {
                ui::draw_pad_hints(960, 620, {{ui::PadButton::Circle, T("Avbryt")}}, 1, 28);
            } else if (login_screen) {
                // Form on the left, QR on the right; without a private address, the form only.
                const bool form_focused = login_view.focus != ui::LoginView::Focus::Qr;
                const float left = qr_possible ? 200.f : 560.f, width = 800;
                const std::string rows[] = {std::string(T("Serveradresse")) + ": " + server,
                    std::string(T("Brukernavn")) + ": " + username,
                    std::string(T("Passord")) + (password.empty() ? ": —" : show_password ? ": " + password : ": ********"),
                    show_password ? T("Skjul passord") : T("Vis passord"), T("Koble til")};
                const int row = static_cast<int>(form_focused ? login_view.focus : login_view.row);
                if (form_focused) login_drop.to({left, 352.f + row * 72, width, 62}, row);
                else login_drop.hide();
                login_drop.draw(dt, 1, &moving, 22);
                for (int i = 0; i < 5; ++i)
                    gfx::text(left + 36, 393.f + i * 72, rows[i], {i == 4 ? gfx::SemiBold : gfx::Medium, 30, width - 72}, ui::kText);
                if (form_focused) ui::draw_pad_hints(left + 36, 760, {{ui::PadButton::Cross, T("Velg")}}, 0, 26);
                if (qr_possible) pairing_view.draw(pairing_now(), dt, 1360, !form_focused);
            }
            // One status line: the current note, else what applies to this screen.
            const char *status = !note.empty() ? note.c_str()
                : !login_screen || scheme_popup || warning ? ""
                : consent && server.compare(0, 7, "http://") == 0 ? T("HTTP sender innlogging og musikk ukryptert.")
                : T("Lagres ukryptert på konsollen: API-nøkkel om mulig, ellers passord.");
            gfx::text(960, 885, status, {gfx::Medium, 24, 1400}, ui::kText2, 1);
        }
        if (connected && !note.empty()) gfx::text(960, 900, note, {gfx::Medium, 24, 1400}, ui::kText, 1);
        if (!connected) gfx::text(960, 950, jf::kDiagBuild, {gfx::Medium, 22}, ui::kText2, 1);
        if (!connected && !diagnostic.empty()) gfx::text(960, 985, diagnostic, {gfx::Medium, 20, 1800}, ui::kText2, 1);
#ifdef NAVI5_DEV_TOOLS
        char import_line[256];
        std::snprintf(import_line, sizeof import_line, "DEV import=%d/%d private=%d/%d cleanup=%d reader=%d/%d bits=%d bad=%d mode=%o links=%llu uid=%u/%u size=%lld delivered=%u busy=%d",
                      temporary_status.stage, temporary_status.error, private_status.stage, private_status.error,
                      int(cleanup_failed), reader_check->stage.load(), reader_check->error.load(), reader_check->bits.load(),
                      reader_check->mismatch.load(), reader_check->mode.load(), reader_check->nlink.load(),
                      reader_check->uid.load(), reader_check->expected_uid.load(), reader_check->size.load(),
                      delivered, int(mailbox_busy));
        if (!connected) gfx::text(960, 1020, import_line, {gfx::Medium, 20, 1800}, ui::kText2, 1);
        if (!connected) gfx::text(960, 1050, jf::live_progress.line(), {gfx::Medium, 20, 1800}, ui::kText2, 1);
        if (!connected) gfx::text(960, 1076, jf::live_progress.ring_line(), {gfx::Medium, 18, 1880}, ui::kText2, 1);
        // Recent requests (all threads) while a library page loads or failed to.
        bool library_trouble = music.state().status() != ui::music::Status::Ready;
        for (const auto &section : music.state().page().sections) library_trouble = library_trouble || !section.ok;
        if (connected && !player_view.visible && library_trouble) {
            const std::string rows = jf::request_log.lines();
            float y = 700;
            for (size_t at = 0; at < rows.size();) {
                const size_t end = rows.find('\n', at);
                gfx::text(140, y, rows.substr(at, end - at), {gfx::Medium, 20, 1700}, ui::kText2);
                y += 28;
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
#endif
        gfx::collect(); gfx::end_frame();
    }
}
