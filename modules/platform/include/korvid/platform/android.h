// Korvid platform on Android: what the app's entry tells the layer below it. An Android
// app's files are not beside an executable: they are entries of its installed package, and
// what it writes goes to directories only the activity knows. platform_hid's entry
// (src/android/) calls this once, before the game's main runs; a program that never calls
// it is a test under adb's shell and gets Linux's answers.
#pragma once

namespace kv {

// package: the installed .apk's path (Activity.getPackageResourcePath). internal: the
// app's private files directory, where saves go. Both are copied.
void platform_android_app(const char* package, const char* internal);

// Whether the app is in the foreground (resumed), and one listener for when that changes:
// the entry calls platform_android_foreground from the activity's resume and pause, on the
// OS's thread, and the listener (the audio device's, which stops its stream while the app is
// away) runs there. Null removes it.
void platform_android_foreground(bool foreground);
void platform_android_on_foreground(void (*fn)(bool foreground));

} // namespace kv
