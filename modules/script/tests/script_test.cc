#include "korvid/script/script.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include <string.h>

// The kv libraries, from Lua: each check is a chunk that returns true.

namespace {
using namespace kv;

bool lua_true(TestCtx& c, lua_State* L, const char* chunk) {
    lua_pushcfunction(L, script_traceback);
    if (luaL_loadstring(L, chunk) != LUA_OK || lua_pcall(L, 0, 1, -2) != LUA_OK) {
        KV_ERROR("test", "%s: %s", c.suite, lua_tostring(L, -1));
        lua_pop(L, 2);
        return false;
    }
    bool ok = lua_toboolean(L, -1);
    lua_pop(L, 2);
    return ok;
}

// A ktable with one component of every shape a field takes, and one with two components
// that share a field name.
struct Asset {
    char   path[24];
    u64    size;
    i32    line;
    u16    flags;
    bool   dirty;
    f32    weight;
    f64    when;
    Handle source;
    u8     digest[4];
    f32    pivot[2];
};
struct Extra { u32 size; u8 small; };

constinit const FieldDesc k_asset_fields[] = {KV_FIELDS(Asset, path, size, line, flags, dirty, weight, when, source, digest, pivot)};
constinit const FieldDesc k_extra_fields[] = {KV_FIELDS(Extra, size, small)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Asset, k_asset_fields), KV_COMPONENT(Extra, k_extra_fields)};
constinit const KTableDesc k_ktables[] = {
    {.name = "st.asset", .components = "Asset", .capacity = 4},
    {.name = "st.both",  .components = "Asset, Extra", .capacity = 4},
};
KV_MODULE_DEFINE(k_st) = {.name = "st", .components = k_components, .ktables = k_ktables};


// Events by their field tables (KR18): a type with defaults and one without a table;
// systems that run Lua inside their units; a cosmetic resource read by its fields.
struct SePlay { u32 track = 7; f32 gain = 1.0f; char title[16] = "untitled"; i16 offset = -1; };
struct SeTick { u8 n; };
struct SeTransport { f64 position; u32 state; };
constinit const FieldDesc k_play_fields[] = {KV_FIELDS(SePlay, track, gain, title, offset)};
constinit const EventTypeDesc k_se_events[] = {KV_EVENT_EX(SePlay, k_play_fields, 0), KV_EVENT(SeTick)};
constinit const FieldDesc k_transport_fields[] = {KV_FIELDS(SeTransport, position, state)};
constinit const ResourceDesc k_se_resources[] = {{.name = ".transport", .fields = k_transport_fields, .cosmetic = true}};
SeTransport g_transport{2.5, 3};
lua_State* g_L = nullptr;
const char* g_emit_chunk = nullptr; // what the emitting system runs this tick
u32 g_seen = 0, g_seen_track = 0;

// Runs `chunk` with the global `ev` pointed at this unit's context; a failure is logged and
// left in the global `err`.
void se_run(TickContext& ctx, const char* chunk) {
    lua_getglobal(g_L, "ev");
    script_events_set_context(g_L, -1, &ctx);
    lua_pop(g_L, 1);
    if (luaL_dostring(g_L, chunk) != LUA_OK) { lua_setglobal(g_L, "err"); }
    lua_getglobal(g_L, "ev");
    script_events_set_context(g_L, -1, nullptr);
    lua_pop(g_L, 1);
}
void se_emit(TickContext& ctx) { if (g_emit_chunk) se_run(ctx, g_emit_chunk); }
void se_read(TickContext& ctx) {
    se_run(ctx, "local got = ev:received('SePlay') SEEN = (SEEN or 0) + #got for _, p in ipairs(got) do LAST = p end");
    // The C++ side of the same batch, to check the bytes the table made.
    for (const SePlay& p : ctx.received<SePlay>()) { ++g_seen; g_seen_track = p.track; }
}
bool se_startup(Engine& e, ModuleInstance& self) { return engine_publish(e, self, ".transport", &g_transport); }
constinit const SystemDesc k_se_systems[] = {
    {.name = ".emit", .phase = Phase::Sim, .fn = se_emit, .emits = "SePlay"},
    {.name = ".read", .phase = Phase::Sim, .fn = se_read, .reads = "SePlay"},
};
KV_MODULE_DEFINE(k_se) = {.name = "se", .events = k_se_events, .resources = k_se_resources, .systems = k_se_systems, .startup = se_startup};

} // namespace

#define LUA_CHECK(c, L, chunk) (c).check(lua_true(c, L, chunk), chunk, __FILE__, __LINE__)

void test_script(TestCtx& c) {
    c.begin("script");
    lua_State* L = luaL_newstate();
    KV_CHECK(c, L != nullptr);
    luaL_openlibs(L);
    script_open_kv(L);
    lua_setglobal(L, "kv");

    char dir[512];
    KV_CHECK(c, io_make_temp_dir("kv-script", dir, sizeof dir));
    lua_pushstring(L, dir);
    lua_setglobal(L, "DIR");

    // kv.fs
    LUA_CHECK(c, L, "return kv.fs.is_dir(DIR) and not kv.fs.is_file(DIR) and kv.fs.exists(DIR)");
    LUA_CHECK(c, L, "return kv.fs.mkdir(DIR .. '/a/b') and kv.fs.write(DIR .. '/a/b/x.txt', 'hello\\0there') and kv.fs.read(DIR .. '/a/b/x.txt') == 'hello\\0there'");
    LUA_CHECK(c, L, "local v, e = kv.fs.read(DIR .. '/nope') return v == nil and e:find('nope') ~= nil");
    LUA_CHECK(c, L, "kv.fs.write(DIR .. '/a/y.txt', 'y') local l = kv.fs.list(DIR) return #l == 2 and l[1] == 'a/b/x.txt' and l[2] == 'a/y.txt'");
    LUA_CHECK(c, L, "return kv.fs.remove(DIR .. '/a/y.txt') and not kv.fs.exists(DIR .. '/a/y.txt') and kv.fs.remove_tree(DIR .. '/a') and not kv.fs.exists(DIR .. '/a')");

    // kv.sha256, kv.random
    LUA_CHECK(c, L, "return kv.sha256('abc') == 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'");
    LUA_CHECK(c, L, "local a, b = kv.random(7), kv.random(7) local same = true for i = 1, 50 do same = same and a:randint(1, 1000) == b:randint(1, 1000) end return same");
    LUA_CHECK(c, L, "local r = kv.random(1) local x = r:random() return x >= 0 and x < 1 and r:choice({'a'}) == 'a' and r:randrange(1) == 0");

    // kv.json, writing: as before the move.
    LUA_CHECK(c, L, "return kv.json.encode(kv.json.object('b', 1, 'a', {1, 2.5, 'x'}, 'n', kv.json.null)) == '{\"b\": 1, \"a\": [1, 2.5, \"x\"], \"n\": null}'");
    LUA_CHECK(c, L, "return kv.json.encode({}, {indent = 2}) == '[]' and kv.json.encode({1, kv.json.object('k', true)}, {indent = 2}) == '[\\n  1,\\n  {\\n    \"k\": true\\n  }\\n]'");
    LUA_CHECK(c, L, "return kv.json.encode(1.0) == '1.0' and kv.json.encode(0.1) == '0.1' and kv.json.encode(1e20) == '1e+20' and kv.json.encode('\\u{e9}') == '\"\\\\u00e9\"'");

    // kv.json, reading: the same values, keys in the order written, lines kept.
    LUA_CHECK(c, L, R"(
        T = '{\n  "meta": { "type": "sprite", "pivot": [0.5, 0.8] },\n  "rules": [\n    { "match": "*.png", "meta": { "cook": false } },\n    { "match": "b/**",\n      "meta": { "tags": ["x"], "n": null, "i": 12, "f": 1.5, "big": 12345678901234567890 } }\n  ]\n}'
        V = assert(kv.json.decode(T))
        return kv.json.is_object(V) and V.meta.type == 'sprite' and V.meta.pivot[2] == 0.8 and #V.rules == 2 and V.rules[1].meta.cook == false)");
    LUA_CHECK(c, L, "local m = V.rules[2].meta return m.n == kv.json.null and math.type(m.i) == 'integer' and m.i == 12 and math.type(m.f) == 'float' and math.type(m.big) == 'float'");
    LUA_CHECK(c, L, "local k = kv.json.keys(V.rules[2].meta) return table.concat(k, ',') == 'tags,n,i,f,big' and table.concat(kv.json.keys(V), ',') == 'meta,rules'");
    LUA_CHECK(c, L, "local seen = {} for k, v in pairs(V.meta) do seen[#seen + 1] = k end return table.concat(seen, ',') == 'type,pivot'");
    LUA_CHECK(c, L, "return kv.json.line(V) == 1 and kv.json.line(V, 'meta') == 2 and kv.json.line(V, 'rules') == 3 and kv.json.line(V.rules) == 3 and kv.json.line(V.rules, 1) == 4 and kv.json.line(V.rules, 2) == 5");
    LUA_CHECK(c, L, "return kv.json.line(V.rules[2]) == 5 and kv.json.line(V.rules[2], 'match') == 5 and kv.json.line(V.rules[2], 'meta') == 6 and kv.json.line(V.rules[2].meta, 'tags') == 6 and kv.json.line({}) == nil and not kv.json.is_object({})");
    // Read, then written, then read again: the same text both times.
    LUA_CHECK(c, L, "local a = kv.json.encode(V) local b = kv.json.encode(assert(kv.json.decode(a))) return a == b and a:find('\"meta\": {\"type\": \"sprite\"', 1, true) == 2");
    // Scalars and empties at the top.
    LUA_CHECK(c, L, "return kv.json.decode('3') == 3 and kv.json.decode('\"s\"') == 's' and kv.json.decode('null') == kv.json.null and kv.json.decode('true') == true and #kv.json.decode('[]') == 0 and kv.json.is_object(kv.json.decode('{}'))");
    // A mistake comes back as nil, what is wrong, and where.
    LUA_CHECK(c, L, "local v, e, l = kv.json.decode('{\\n  \"a\": 1,\\n  \"b\" 2\\n}') return v == nil and e == \"expected ':'\" and l == 3");
    // Comments are refused unless asked for; a \"//\" member is kept unless dropped.
    LUA_CHECK(c, L, R"(
        local t = '// top\n{\n  "//": "why this file exists",\n  "meta": { "//": ["a", "list"], "type": "x" } // trailing\n}'
        local plain = kv.json.decode(t)
        local kept = assert(kv.json.decode(t, { comments = true }))
        local v = assert(kv.json.decode(t, { comments = true, drop = '//' }))
        return plain == nil and kept['//'] == 'why this file exists' and v['//'] == nil and v.meta['//'] == nil and v.meta.type == 'x'
           and table.concat(kv.json.keys(v), ',') == 'meta' and kv.json.line(v, 'meta') == 4)");
    // A repeated name keeps its first place and its last value.
    LUA_CHECK(c, L, "local v = kv.json.decode('{\"a\": 1, \"b\": 2, \"a\": 3}') return v.a == 3 and table.concat(kv.json.keys(v), ',') == 'a,b'");

    // kv.zip opens (and refuses what is not one).
    LUA_CHECK(c, L, "kv.fs.write(DIR .. '/not.zip', 'not a zip at all') return not pcall(kv.zip.open, DIR .. '/not.zip')");

    lua_close(L);
    io_remove_tree(dir);
}

void test_script_ktable(TestCtx& c) {
    c.begin("script ktable");
    const ModuleDesc* const list[] = {&k_st};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KTable* assets = engine_ktable(e, "st.asset");
    KTable* both   = engine_ktable(e, "st.both");
    KV_CHECK(c, assets && both);

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    script_push_ktable(L, assets);
    lua_setglobal(L, "assets");
    script_push_ktable(L, both);
    lua_setglobal(L, "both");

    LUA_CHECK(c, L, "return assets:name() == 'st.asset' and assets:capacity() == 4 and assets:live() == 0 and #assets:fields() == 10 and assets:fields()[1] == 'Asset.path'");

    // A row spawned from Lua is the engine's row: zero, then what was written.
    LUA_CHECK(c, L, "H = assets:spawn() return math.type(H) == 'integer' and assets:valid(H) and assets:live() == 1 and assets:get(H, 'size') == 0 and assets:get(H, 'path') == '' and assets:get(H, 'dirty') == false");
    LUA_CHECK(c, L, R"(
        assets:set(H, 'path', 'kenney/ships.png') assets:set(H, 'size', 123456789012) assets:set(H, 'line', -7) assets:set(H, 'flags', 65535)
        assets:set(H, 'dirty', true) assets:set(H, 'weight', 0.5) assets:set(H, 'when', 1e300) assets:set(H, 'source', H)
        assets:set(H, 'digest', '\1\2\0\255') assets:set(H, 'pivot', {0.5, 0.25})
        return true)");
    Handle h{0};
    for (u32 i = 0; i < assets->rows(); ++i) if (assets->is_alive(i)) h = assets->handle_at(i);
    const Asset* a = assets->get<Asset>(h);
    KV_CHECK(c, a && strcmp(a->path, "kenney/ships.png") == 0 && a->size == 123456789012ull && a->line == -7 && a->flags == 65535 && a->dirty);
    KV_CHECK(c, a && a->weight == 0.5f && a->when == 1e300 && a->source == h && a->digest[0] == 1 && a->digest[2] == 0 && a->digest[3] == 255 && a->pivot[1] == 0.25f);
    LUA_CHECK(c, L, "return assets:get(H, 'path') == 'kenney/ships.png' and assets:get(H, 'size') == 123456789012 and assets:get(H, 'line') == -7 and assets:get(H, 'source') == H and assets:get(H, 'when') == 1e300");
    LUA_CHECK(c, L, "local d, p = assets:get(H, 'digest'), assets:get(H, 'pivot') return d == '\\1\\2\\0\\255' and #p == 2 and p[1] == 0.5 and p[2] == 0.25 and assets:get(H, 'Asset.flags') == 65535");

    // What C++ writes, Lua reads; a row object reads and writes by name.
    assets->get<Asset>(h)->size = 42;
    LUA_CHECK(c, L, "local r = assets:row(H) return r.size == 42 and r.handle == H and r.alive and r.path == 'kenney/ships.png'");
    LUA_CHECK(c, L, "local r = assets:row(H) r.size = 43 r.path = 'b.png' r.dirty = false return assets:get(H, 'size') == 43 and r.path == 'b.png' and r.dirty == false");
    KV_CHECK(c, assets->get<Asset>(h)->size == 43 && strcmp(assets->get<Asset>(h)->path, "b.png") == 0 && assets->get<Asset>(h)->path[6] == 0);

    // Spawn with fields; iteration in slot order; a full ktable says nil.
    LUA_CHECK(c, L, "local h2 = assets:spawn{ path = 'c.png', size = 9, pivot = {1, 2} } return assets:get(h2, 'size') == 9 and assets:get(h2, 'pivot')[2] == 2 and assets:live() == 2");
    LUA_CHECK(c, L, "local seen = {} for h in assets:each() do seen[#seen + 1] = assets:get(h, 'path') end return table.concat(seen, ',') == 'b.png,c.png'");
    LUA_CHECK(c, L, "local a, b, x = assets:spawn(), assets:spawn(), assets:spawn() local full = a and b and x == nil assets:destroy(a) assets:destroy(b) return full and assets:live() == 2");
    LUA_CHECK(c, L, "local ok, e = pcall(assets.spawn, assets, { nope = 1 }) return not ok and e:find(\"no field 'nope'\") ~= nil and assets:live() == 2");

    // Mistakes are errors, not quiet damage.
    LUA_CHECK(c, L, "local ok, e = pcall(assets.set, assets, H, 'flags', 65536) return not ok and e:find('does not fit') ~= nil and assets:get(H, 'flags') == 65535");
    LUA_CHECK(c, L, "return not pcall(assets.set, assets, H, 'size', 'big') and not pcall(assets.set, assets, H, 'size', 1.5) and not pcall(assets.set, assets, H, 'weight', 'x')");
    LUA_CHECK(c, L, "return not pcall(assets.set, assets, H, 'path', string.rep('x', 24)) and pcall(assets.set, assets, H, 'path', string.rep('x', 23)) and not pcall(assets.set, assets, H, 'path', 5)");
    LUA_CHECK(c, L, "return not pcall(assets.set, assets, H, 'digest', 'abc') and not pcall(assets.set, assets, H, 'pivot', {1}) and not pcall(assets.get, assets, H, 'nope')");
    LUA_CHECK(c, L, "local r = assets:row(H) return not pcall(function() return r.nope end) and not pcall(function() r.nope = 1 end)");

    // A destroyed row: get is nil, set and a kept row object are errors, the handle is stale.
    LUA_CHECK(c, L, "R = assets:row(H) return assets:destroy(H) and not assets:valid(H) and assets:get(H, 'size') == nil and assets:row(H) == nil and not assets:destroy(H)");
    LUA_CHECK(c, L, "return not pcall(assets.set, assets, H, 'size', 1) and not pcall(function() return R.size end) and R.alive == false and not assets:valid('x')");
    LUA_CHECK(c, L, "local h3 = assets:spawn() return h3 ~= H and assets:get(h3, 'size') == 0 and assets:get(h3, 'path') == ''");

    // Two components: a shared field name needs its component; the rest do not.
    LUA_CHECK(c, L, "local h = both:spawn{ ['Asset.size'] = 5, ['Extra.size'] = 6, small = 7, line = 8 } B = h return both:get(h, 'Asset.size') == 5 and both:get(h, 'Extra.size') == 6 and both:get(h, 'small') == 7 and both:get(h, 'Extra.small') == 7");
    LUA_CHECK(c, L, "return not pcall(both.get, both, B, 'size') and #both:fields() == 12");
    KV_CHECK(c, both->live() == 1);

    lua_close(L);
    engine_shutdown(e);
}

void test_script_events(TestCtx& c) {
    c.begin("script events");
    const ModuleDesc* const list[] = {&k_se};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    g_L = L;
    script_push_events(L, e);
    lua_setglobal(L, "ev");

    // What the object knows, outside the schedule.
    LUA_CHECK(c, L, "local f = ev:fields('SePlay') return ev:has('SePlay') and not ev:has('Nope') and #f == 4 and f[1] == 'track' and f[3] == 'title' and #ev:fields('SeTick') == 0");
    LUA_CHECK(c, L, "local ok, m = pcall(ev.emit, ev, 'Nope', {}) return not ok and m:find(\"no event type 'Nope'\") ~= nil");

    // Emitted inside a unit and read by the next, the rest at the struct's defaults.
    g_emit_chunk = "ev:emit('SePlay', { title = 'Blue in Green', gain = 0.5 })";
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, g_seen == 1 && g_seen_track == 7);
    LUA_CHECK(c, L, "return SEEN == 1 and LAST.track == 7 and LAST.gain == 0.5 and LAST.title == 'Blue in Green' and LAST.offset == -1 and err == nil");
    g_emit_chunk = "ev:emit('SePlay')";
    engine_frame(e, e.clock.tick_dt);
    LUA_CHECK(c, L, "return SEEN == 2 and LAST.track == 7 and LAST.gain == 1.0 and LAST.title == 'untitled'");

    // A row's mistakes, a type's and an undeclared emit or read, each a Lua error the unit
    // can catch; nothing half-built is emitted.
    g_emit_chunk = R"(
        local function fails(f, want) local ok, m = pcall(f) return not ok and m:find(want, 1, true) ~= nil end
        OK = fails(function() ev:emit('SePlay', { track = -1 }) end, 'does not fit')
         and fails(function() ev:emit('SePlay', { gain = 'loud' }) end, 'a number expected')
         and fails(function() ev:emit('SePlay', { title = string.rep('x', 16) }) end, 'does not fit char[16]')
         and fails(function() ev:emit('SePlay', { tempo = 1 }) end, "SePlay has no field 'tempo'")
         and fails(function() ev:emit('SeTick', { n = 1 }) end, 'has no field table')
         and fails(function() ev:emit('SeTick') end, 'without declaring it in `emits`')
         and fails(function() ev:received('SePlay') end, 'without declaring it in `reads`'))";
    engine_frame(e, e.clock.tick_dt);
    LUA_CHECK(c, L, "return OK == true and SEEN == 2 and err == nil");
    KV_CHECK(c, g_seen == 2);

    // From outside the schedule: into the engine's writer, delivered at the next boundary.
    g_emit_chunk = nullptr;
    LUA_CHECK(c, L, "ev:emit('SePlay', { track = 42 }) return true");
    engine_frame(e, e.clock.tick_dt);
    KV_CHECK(c, g_seen == 3 && g_seen_track == 42);
    LUA_CHECK(c, L, "return SEEN == 3 and LAST.track == 42");

    // A type registered while the engine runs (KR19), from a table made here: emitted by
    // name from inside a unit, through the engine's writer, and listened to; not received.
    {
        static FieldDesc line_fields[2] = {{"n", FieldType::U32, 1, 0}, {"text", FieldType::Char, 12, 4}};
        EventTypeDesc d{};
        d.name = "SeLine";
        d.size = 16;
        d.align = 4;
        d.fields = Span<const FieldDesc>(line_fields, 2);
        u32 id = engine_add_event_type(e, "se", d);
        KV_CHECK(c, id != 0);
        static u32 heard = 0, heard_n = 0;
        engine_listen(e, id, [](void*, u32, const void* ev) { ++heard; memcpy(&heard_n, ev, 4); }, nullptr);
        g_emit_chunk = "ev:emit('SeLine', { n = 5, text = 'la la' }) OK2 = not pcall(ev.received, ev, 'SeLine')";
        engine_frame(e, e.clock.tick_dt);
        g_emit_chunk = nullptr;
        engine_frame(e, e.clock.tick_dt);
        KV_CHECK(c, heard == 1 && heard_n == 5);
        LUA_CHECK(c, L, "return ev:has('SeLine') and #ev:fields('SeLine') == 2 and OK2 == true and err == nil");
    }

    // A cosmetic ktable added while the engine runs (KR19): rows by field from Lua.
    {
        static FieldDesc word_fields[2] = {{"at", FieldType::F64, 1, 0}, {"word", FieldType::Char, 16, 8}};
        static ComponentDesc word{};
        word.name = "Word"; word.size = 24; word.align = 8; word.fields = Span<const FieldDesc>(word_fields, 2);
        RuntimeKTableDesc kd;
        kd.name = "se.words";
        kd.components = Span<const ComponentDesc>(&word, 1);
        kd.capacity = 8;
        u32 kid = engine_add_ktable(e, "se", kd);
        KV_CHECK(c, kid != 0);
        script_push_ktable(L, engine_ktable(e, kid));
        lua_setglobal(L, "words");
        LUA_CHECK(c, L, "local h = words:spawn{ at = 1.5, word = 'hello' } return words:get(h, 'word') == 'hello' and words:get(h, 'Word.at') == 1.5 and words:live() == 1 and words:name() == 'se.words'");
        LUA_CHECK(c, L, "return not pcall(words.spawn, words, { word = string.rep('x', 16) })");
    }

    // A cosmetic resource by its fields, as of now; nothing for a name without a table.
    KV_CHECK(c, script_push_resource(L, e, "se.transport"));
    lua_setglobal(L, "T");
    LUA_CHECK(c, L, "return T.position == 2.5 and T.state == 3");
    KV_CHECK(c, !script_push_resource(L, e, "se.nothing") && lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_close(L);
    g_L = nullptr;
    engine_shutdown(e);
}

int main() {
    TestCtx c;
    test_script(c);
    test_script_ktable(c);
    test_script_events(c);
    names_shutdown();
    mem_shutdown();
    return test_report(c, "script_tests");
}
