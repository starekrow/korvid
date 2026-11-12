// Korvid audio: module record.
//
//   handle kind  audio.voice
//   events       VoiceFinished{voice}
//   resources    audio.commands        declare `appends` on it to call audio_play and friends
//   systems      audio.extract         PostSim, main thread, Always: commands to the mixer,
//                                      finished voices back, pause and resume with the sim
//
// Startup opens the platform's audio device at 48 kHz stereo; without one the module runs
// silent and voices finish on the tick after they start. See the module's README.
#pragma once
#include "korvid/audio/audio.h"
#include "korvid/core/module.h"

namespace kv {

KV_MODULE_DECLARE(audio_module);

} // namespace kv
