// Korvid platform: the settings store (KR24). Named values a program keeps between runs
// (a session's volume, the face chosen, a window's place) in one file, settings.json, in
// the user's settings directory (platform_user_dir's Settings), where a person who looks
// finds them.
//
//   SettingsStore* st = platform_settings(e, "starekrow", "blackwing");
//   f64 volume = settings_number(st, "volume", 0.8);
//   settings_set_number(st, "volume", 0.5);      // written soon, off this thread
//   settings_struct(st, "window", k_window_fields, &win);   // a struct by its field table
//
// - Read whole when opened, before anyone asks: a read is a lookup, never I/O.
// - Written whole, off the calling thread, soon after a set: to a temporary file, flushed
//   to the disk, and renamed over the old. A crash, a kill or a full disk leaves the old
//   file or the new one, never half of either.
// - Two programs (or two runs) writing the same store do not interleave: a write holds a
//   lock on settings.lock beside it between processes, reads the file again under it and
//   writes what it found with this store's changes over it, the last writer winning by
//   key. What another program wrote is seen at the next open.
// - Values: an integer, a number, a string, or a struct by its field table (component.h),
//   which is a JSON object of its fields; any other JSON a person wrote is kept as it is.
// - One thread uses a store (the main one, usually); the writer is its own.
// - Not sized for bulk: a queue of ten thousand paths is a file of its own.
#pragma once
#include "korvid/core/component.h"

namespace kv {

struct Engine;
struct SettingsStore;

// The store in `dir` (settings.json there, made by the first write), read now. Null, with
// *err, only when the directory cannot be used; a missing or spoiled file is an empty store
// (a spoiled one logged, and replaced by the first write).
SettingsStore* settings_open_dir(const char* dir, i32* err = nullptr);
// Writes what is pending, waits for it, and frees the store.
void settings_close(SettingsStore* s);
// Waits until everything set so far is on the disk. False if a write failed since the
// last flush (logged; the values stay pending, and the next set tries again).
bool settings_flush(SettingsStore* s);

bool        settings_has(const SettingsStore* s, const char* key);
i64         settings_int(const SettingsStore* s, const char* key, i64 fallback);
f64         settings_number(const SettingsStore* s, const char* key, f64 fallback); // an integer too
// Valid until the key is set or removed, or the store closed.
const char* settings_string(const SettingsStore* s, const char* key, const char* fallback);
// The fields the stored object has, into `out`; the rest are left as they were. False,
// `out` untouched, when there is no such key or it is not an object.
bool        settings_struct(const SettingsStore* s, const char* key, Span<const FieldDesc> fields, void* out);

void settings_set_int(SettingsStore* s, const char* key, i64 v);
void settings_set_number(SettingsStore* s, const char* key, f64 v);
void settings_set_string(SettingsStore* s, const char* key, const char* v);
void settings_set_struct(SettingsStore* s, const char* key, Span<const FieldDesc> fields, const void* v);
void settings_remove(SettingsStore* s, const char* key);

// The program's store, opened in the settings directory for (org, app) at the first call
// and closed (written) at the platform module's shutdown. Null if there is no settings
// directory.
SettingsStore* platform_settings(Engine& e, const char* org, const char* app);

} // namespace kv
