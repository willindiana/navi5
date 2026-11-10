/* Navi5, adapted from Nuvio PS5, Copyright (C) 2026 Husam Osman.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * PCM session state. No playback threads start in placeholder shell.
 */
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include "evo_packet_queue.h"
int g_ps5_user_id = 0;
int screen = 0, player_paused = 0, audio_stream_index = -1;
int evo_audio_channels = 2;
volatile int pb_prebuffer_hold = 0, pb_scrub_hold = 0;
AVFormatContext *play_fmt = nullptr;
AVCodecContext *audio_ctx = nullptr;
PacketQueue audio_packet_queue;
double resume_base_offset_seconds = 0, media_duration_sec = 0, requested_resume_seek_pos = 0;
long long controls_last_used_ms = 0;
char current_media_path[512] = {0};
int prospero_get_initial_user_id(void) { return g_ps5_user_id; }
}
