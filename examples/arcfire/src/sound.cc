// arcfire: what the game sounds like. Every effect is a row of the table below: its
// file, its volume, its priority (the audio module's: under load a busy voice of no higher
// priority is taken, and a sound asked for twice in a tick starts once), and how far its
// pitch wanders, so a hundred shots do not ring as one. The volley and the grazes are the
// lowest; deaths, the bomb, the egg and a boss's fall the highest. Everything but the
// music pauses with the sim and follows its time scale in pitch, so the egg's slow time
// drops an octave.
//
// The music: the stage's track, crossfading to the boss's at a boss mark and back once
// the boss has died or left.
#include "arcfire.h"
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/asset/sound.h"
#include "korvid/audio/audio.h"

namespace kv::arcfire {

namespace {

struct SfxDesc { const char* file; f32 volume; u8 priority; f32 wander; };
constinit const SfxDesc k_sfx[SFX_COUNT] = {
    {"shot",      0.22f, 0, 0.06f},
    {"hit",       0.30f, 1, 0.10f},
    {"ping",      0.20f, 0, 0.10f},
    {"kill",      0.45f, 1, 0.08f},
    {"kill_big",  0.60f, 2, 0.06f},
    {"boss_down", 0.90f, 3, 0.00f},
    {"rumble",    0.80f, 3, 0.00f},
    {"graze",     0.18f, 0, 0.12f},
    {"chip",      0.22f, 0, 0.10f},
    {"bomb",      0.80f, 3, 0.00f},
    {"egg_break", 0.75f, 3, 0.00f},
    {"egg_open",  0.60f, 3, 0.00f},
    {"egg_bloom", 0.70f, 3, 0.00f},
    {"warning",   0.80f, 3, 0.00f},
};
constexpr f32 kMusicVolume = 0.55f;

void sounds_load(Engine& e, Game& g) {
    for (u32 i = 0; i < SFX_COUNT; ++i) {
        char path[96];
        format(path, sizeof path, "arcfire/sound/%s.ogg", k_sfx[i].file);
        g.sfx[i] = asset_load_sound(e, path, asset_scope_global(e));
    }
    // Each stage's two tracks, by k_stage_names' order: Shoreline's, Orbit's.
    static const char* const k_tracks[kStageCount][2] = {{"stage", "boss"}, {"orbit", "orbit_boss"}};
    for (u32 i = 0; i < kStageCount; ++i) {
        char path[96];
        format(path, sizeof path, "arcfire/music/%s.wav", k_tracks[i][0]);
        g.music_stage[i] = asset_load_sound(e, path, asset_scope_global(e));
        format(path, sizeof path, "arcfire/music/%s.wav", k_tracks[i][1]);
        g.music_boss[i] = asset_load_sound(e, path, asset_scope_global(e));
    }
}

} // namespace

void sfx(TickContext& ctx, Game& g, Sfx which, f32 pitch) {
    const SfxDesc& d = k_sfx[which];
    SoundPlay p;
    p.sound = g.sfx[which];
    p.volume = d.volume;
    p.pitch = pitch * (d.wander > 0.0f ? 1.0f + g.fx.range(-d.wander, d.wander) : 1.0f);
    p.priority = d.priority;
    audio_play(ctx, p);
}

void music(TickContext& ctx, Game& g, bool boss) {
    MusicPlay m;
    m.fade_out = boss ? 1.0f : 2.0f;
    m.fade_in = boss ? 0.5f : 2.0f;
    m.overlap = boss ? 0.5f : 1.0f;
    m.volume = kMusicVolume;
    music_play(ctx, boss ? g.music_boss[g.stage_at] : g.music_stage[g.stage_at], m);
}

// ---- what this file declares (its part of the module: module.cc lists it) -----------------------
namespace sound_part {
// The effects and the music.
bool startup(Engine& e, ModuleInstance& self) {
    Game& g = *self.state_as<Game>();
    sounds_load(e, g);
    return true;
}
} // namespace sound_part
KV_MODULE_PART(arcfire_sound) = {.startup = sound_part::startup};

} // namespace kv::arcfire
