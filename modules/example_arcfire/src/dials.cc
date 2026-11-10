// example_arcfire: the dials. Every count the game has is a dial, and a position of the
// dials is a way to run it: Normal (the moderate default), Stress (every count up, the
// ship invulnerable under the bot, the stage looping) and Demo (Normal, the script on the
// stick, the stage ending). `--dial name=value` moves one dial for a measurement. The
// dials live in the world row, so the hash covers them: another position is another run.
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace kv::arcfire {

namespace {

const char* const k_position_names[] = {"normal", "stress", "demo"};

// The dials by name, for --dial and the log: where each lives in Dials and whether it is
// a count or a scale.
struct DialField { const char* name; u32 offset; bool scale; };
#define ARC_DIAL(m, scale) {#m, (u32)offsetof(Dials, m), scale}
constinit const DialField k_dial_fields[] = {
    ARC_DIAL(rank, true), ARC_DIAL(bullet_speed, true), ARC_DIAL(formation_count, true), ARC_DIAL(formation_spacing, true),
    ARC_DIAL(particles, false),     ARC_DIAL(plane_density, false), ARC_DIAL(enemy_health, true), ARC_DIAL(bot, false),
    ARC_DIAL(invulnerable, false),  ARC_DIAL(loops, false),         ARC_DIAL(scroll_rows, false),
    ARC_DIAL(weapon, false),        ARC_DIAL(power, false),
};
#undef ARC_DIAL

} // namespace

Dials dials_for(Position p) {
    Dials d;
    d.position = (u32)p;
    d.rank = 0.5f;
    d.bullet_speed = 1.0f;
    d.formation_count = 1.0f;
    d.formation_spacing = 1.0f;
    d.particles = 6;
    d.plane_density = 1;
    d.enemy_health = 1.0f;
    d.bot = 0;
    d.invulnerable = 0;
    d.loops = 1;
    d.scroll_rows = 0;
    d.weapon = WeaponSpread;
    d.power = 1;
    switch (p) {
    case Position::Normal: break;
    case Position::Demo: // the demo ends at the stage's end, and dies once: the egg plays, then nothing hits
        d.loops = 0;
        d.invulnerable = 2;
        break;
    case Position::Stress:
        d.rank = 24.0f; // the patterns' densest at 1 is a few thousand bullets; 24 holds twenty thousand through the cross
        d.formation_count = 3.0f;
        d.formation_spacing = 0.5f;
        d.particles = 24;
        d.plane_density = 8;
        d.enemy_health = 2.0f;
        d.bot = 1;
        d.invulnerable = 1;
        break;
    }
    return d;
}

void dials_apply_options(Engine& e, Dials& d) {
    for (u32 i = 0;; ++i) {
        const char* opt = frameloop_option(e, "--dial", i);
        if (!opt) break;
        const char* eq = strchr(opt, '=');
        const DialField* f = nullptr;
        for (const DialField& df : k_dial_fields)
            if (eq && (usize)(eq - opt) == strlen(df.name) && !strncmp(opt, df.name, (usize)(eq - opt))) f = &df;
        if (!f) { KV_WARN("arcfire", "--dial %s: no such dial (name=value)", opt); continue; }
        u8* at = (u8*)&d + f->offset;
        if (f->scale) { f32 v = (f32)atof(eq + 1); memcpy(at, &v, sizeof v); }
        else { u32 v = (u32)strtoul(eq + 1, nullptr, 10); memcpy(at, &v, sizeof v); }
    }
}

void dials_log(const Dials& d) {
    char line[512];
    usize n = (usize)snprintf(line, sizeof line, "dials: %s", k_position_names[d.position < 3 ? d.position : 0]);
    for (const DialField& f : k_dial_fields) {
        if (n >= sizeof line) break;
        const u8* at = (const u8*)&d + f.offset;
        if (f.scale) { f32 v; memcpy(&v, at, sizeof v); n += (usize)snprintf(line + n, sizeof line - n, ", %s %g", f.name, (f64)v); }
        else { u32 v; memcpy(&v, at, sizeof v); n += (usize)snprintf(line + n, sizeof line - n, ", %s %u", f.name, v); }
    }
    KV_INFO("arcfire", "%s", line);
}

} // namespace kv::arcfire
