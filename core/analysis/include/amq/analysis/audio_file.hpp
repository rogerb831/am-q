#pragma once
// Audio-file reference for EQ Match (area 17 R2, R9, R10).
//
// Decoding: Am-Q's own WAV (RIFF/WAVE) reader, so v1 ships no third-party decoder (area 17 Counsel
// task: nothing to review). PCM 16, 24 and 32-bit integer, IEEE float 32 and 64-bit, plain or
// WAVE_FORMAT_EXTENSIBLE, any channel count (channels are merged later by mean power). Files over
// kMaxFileSeconds are refused (memory). Other formats (FLAC, MP3, AIFF) are a later task.
//
// Resampling to the session rate: a windowed-sinc interpolator (Kaiser window, beta 9, 32 zero
// crossings per side; when downsampling the cutoff follows the output Nyquist), standard
// band-limited interpolation (Smith, "Digital Audio Resampling Home Page"). Good to well under
// 0.1 dB across 20 Hz - 20 kHz, which is all a long-term spectrum needs.
//
// Not real-time code: runs on the file-analysis thread.

#include <string>
#include <vector>

namespace amq::analysis {

inline constexpr double kMaxFileSeconds = 20.0 * 60.0;

struct AudioFile {
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels;  // planar
    [[nodiscard]] size_t frames() const noexcept { return channels.empty() ? 0 : channels[0].size(); }
};

// Reads a WAV file. On failure returns false with a message for the user.
bool readWav(const std::string& path, AudioFile& out, std::string* error);
// Parses WAV bytes (the reader's core; tests use it without files).
bool parseWav(const std::vector<unsigned char>& bytes, AudioFile& out, std::string* error);
// Resamples every channel to `rate` (no-op when equal).
void resample(AudioFile& f, double rate);

}  // namespace amq::analysis
