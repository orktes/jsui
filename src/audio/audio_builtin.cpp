// The built-in audio backend. Placeholder until the miniaudio backend lands:
// no built-in backend (sounds need JsuiConfig.on_audio).
#include "audio/audio.h"

namespace jsui {

std::unique_ptr<AudioBackend> createBuiltinAudio(Runtime&) { return nullptr; }

}  // namespace jsui
