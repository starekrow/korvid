// Korvid platform: the directories a program is given. Where it was installed, where its
// assets are, and where it keeps what is its user's.
//
// Every one is named a `_dir`. The two families end differently, and have since before
// they shared a header, so each says which it is:
// - the installed ones and the user's (base, exe, user) end with a separator: a prefix,
//   ready for a name ("%sshaders/sprite.spv");
// - the ones that are a place to work in (assets here; io_current_dir and io_make_temp_dir
//   in io.h) do not: a path, joined with one ("%s/maps/town.tmj").
// Paths are UTF-8 with '/' separators on every OS.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

// Directory of the running executable, with a trailing separator. What the build puts
// beside it lives beneath: shaders/, assets/, its pack. In a macOS .app that is the
// bundle's resources. "" before the platform module is up.
const char* platform_base_dir(Engine& e);
// The same, asked of the OS directly, for code with no engine (a test). False if the OS
// will not say.
bool        platform_exe_dir(char* out, u32 cap);
// Where this program's loose assets are, with no trailing separator: the output tree
// goldberg cooked for it, named by <exe>.assets beside the executable (one line, which
// kormake writes), or assets/ beside the executable when there is no such file (a
// packaged program). Without an engine: for tools, and tests that read a file directly.
// False if neither is a directory.
bool        platform_assets_dir(char* out, u32 cap);

// The three places a program keeps what is its user's (KR24), each under <org>/<app>/,
// created if need be, with a trailing separator; "" if the OS has none.
//   Settings: what the user chose (a session, the face, per-device choices).
//   Data:     what the user made and must not lose (saves).
//   Cache:    what is rebuilt if it goes (an index), which backups leave out.
//            Settings                 Data                            Cache
//   Linux    $XDG_CONFIG_HOME         $XDG_DATA_HOME                  $XDG_CACHE_HOME
//            (~/.config)              (~/.local/share)                (~/.cache)
//   Windows  %APPDATA%                %APPDATA%                       %LOCALAPPDATA%\<org>\<app>\cache
//   Apple    Library/Application Support (macOS and iOS: Preferences    Library/Caches
//            is cfprefsd's, for the defaults system's plists only)
//   Android  the app's files dir      the app's files dir             the app's cache dir
// Where Settings and Data are one directory, the two kinds share it. The first call fixes
// the names for the process.
enum class UserDir : u8 { Settings, Data, Cache };
const char* platform_user_dir(Engine& e, UserDir which, const char* org, const char* app);

} // namespace kv
