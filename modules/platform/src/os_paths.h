// Korvid platform: the directories the OS gives a process, one source per OS.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The running executable's directory with a trailing separator: where the build puts what
// it needs beside it (shaders/, assets/, its pack). In a macOS .app that is the bundle's
// Contents/Resources/, not Contents/MacOS/. False if the OS will not say.
bool os_base_path(char* out, u32 cap);
// The running executable's file name, without ".exe". False where a program has no file
// of its own to name (an Android app).
bool os_exe_name(char* out, u32 cap);
// The per-user writable directory for (org, app), created if missing, with a trailing
// separator. False with a log line if there is no home to put it in.
bool os_pref_path(const char* org, const char* app, char* out, u32 cap);

// The archive the program's own files are inside, when they are not in a directory: on
// Android the installed package (an .apk, a zip), whose entries under "assets/" are what
// a desktop build has beside its executable. Null everywhere else, and for a program that
// is not an app (a test under adb's shell).
#if defined(__ANDROID__)
const char* os_bundle_archive();
#else
inline const char* os_bundle_archive() { return nullptr; }
#endif
constexpr const char* kBundlePrefix = "assets/";
// Lets go of what io_read_bundled keeps open (the archive and its table): the platform
// module's shutdown calls it. The next bundled read opens it again.
void io_bundle_close();

} // namespace kv
