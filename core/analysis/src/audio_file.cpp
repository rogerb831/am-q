#include "amq/analysis/audio_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace amq::analysis {

namespace {

uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

}  // namespace

bool parseWav(const std::vector<unsigned char>& b, AudioFile& out, std::string* error) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return fail("Not a WAV file.");
    uint16_t format = 0, channels = 0, bits = 0, block = 0;
    uint32_t rate = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        const uint32_t size = u32(b.data() + p + 4);
        const unsigned char* body = b.data() + p + 8;
        const size_t avail = std::min<size_t>(size, b.size() - p - 8);
        if (std::memcmp(b.data() + p, "fmt ", 4) == 0 && avail >= 16) {
            format = u16(body), channels = u16(body + 2), rate = u32(body + 4), block = u16(body + 12), bits = u16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = u16(body + 24);  // WAVE_FORMAT_EXTENSIBLE: the sub-format
        } else if (std::memcmp(b.data() + p, "data", 4) == 0) {
            data = body;
            dataSize = avail;
        }
        p += 8 + size + (size & 1);
    }
    if (!data || channels == 0 || rate == 0 || block == 0) return fail("The WAV file has no audio data.");
    const bool isFloat = format == 3, isPcm = format == 1;
    if (!(isPcm && (bits == 16 || bits == 24 || bits == 32)) && !(isFloat && (bits == 32 || bits == 64)))
        return fail("Unsupported WAV format (use PCM 16/24/32-bit or float).");
    const size_t frames = dataSize / block;
    if (static_cast<double>(frames) / rate > kMaxFileSeconds) return fail("The file is longer than 20 minutes.");
    out.sampleRate = rate;
    out.channels.assign(channels, std::vector<float>(frames));
    const int bytes = bits / 8;
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) {
            const unsigned char* s = data + i * block + static_cast<size_t>(c) * bytes;
            float v = 0.0f;
            if (isFloat && bits == 32) {
                std::memcpy(&v, s, 4);
            } else if (isFloat) {
                double d;
                std::memcpy(&d, s, 8);
                v = static_cast<float>(d);
            } else if (bits == 16) {
                v = static_cast<int16_t>(u16(s)) / 32768.0f;
            } else if (bits == 24) {
                int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
                if (x & 0x800000) x |= ~0xFFFFFF;
                v = static_cast<float>(x / 8388608.0);
            } else {
                v = static_cast<float>(static_cast<int32_t>(u32(s)) / 2147483648.0);
            }
            out.channels[static_cast<size_t>(c)][i] = std::isfinite(v) ? v : 0.0f;
        }
    return true;
}

bool readWav(const std::string& path, AudioFile& out, std::string* error) {
    const std::u8string u8(path.begin(), path.end());  // UTF-8 paths (the file dialog's)
    std::ifstream f(std::filesystem::path(u8), std::ios::binary);
    if (!f) {
        if (error) *error = "Could not open the file.";
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseWav(bytes, out, error);
}

void resample(AudioFile& f, double rate) {
    if (f.sampleRate <= 0.0 || rate <= 0.0 || f.sampleRate == rate || f.frames() == 0) return;
    constexpr int kZeros = 32, kPerZero = 1024;
    constexpr double kBeta = 9.0, kPi = 3.14159265358979323846;
    const double ratio = rate / f.sampleRate;
    const double cutoff = std::min(1.0, ratio) * 0.97;  // a little below the lower Nyquist
    // The kernel k(u) = sinc(u) * kaiser(u / kZeros), u in zero crossings, tabulated finely; the
    // filter in input samples is h(x) = cutoff * k(x * cutoff).
    std::vector<double> table(static_cast<size_t>(kZeros * kPerZero + 2), 0.0);
    const double i0b = besselI0(kBeta);
    for (size_t t = 0; t < table.size(); ++t) {
        const double u = static_cast<double>(t) / kPerZero;
        const double r = u / kZeros;
        const double sinc = u < 1e-12 ? 1.0 : std::sin(kPi * u) / (kPi * u);
        table[t] = r >= 1.0 ? 0.0 : sinc * besselI0(kBeta * std::sqrt(1.0 - r * r)) / i0b;
    }
    auto kernel = [&](double x) {
        const double u = std::abs(x) * cutoff * kPerZero;
        const auto t = static_cast<size_t>(u);
        if (t + 1 >= table.size()) return 0.0;
        const double fr = u - static_cast<double>(t);
        return cutoff * (table[t] + fr * (table[t + 1] - table[t]));
    };
    const double half = kZeros / cutoff;  // half-length in input samples
    const auto outFrames = static_cast<size_t>(std::floor(f.frames() * ratio));
    for (auto& ch : f.channels) {
        std::vector<float> y(outFrames);
        const auto last = static_cast<long long>(ch.size()) - 1;
        for (size_t n = 0; n < outFrames; ++n) {
            const double t = n / ratio;  // position in input samples
            const auto lo = std::max(0LL, static_cast<long long>(std::ceil(t - half)));
            const auto hi = std::min(last, static_cast<long long>(std::floor(t + half)));
            double acc = 0.0;
            for (long long k = lo; k <= hi; ++k) acc += ch[static_cast<size_t>(k)] * kernel(t - static_cast<double>(k));
            y[n] = static_cast<float>(acc);
        }
        ch = std::move(y);
    }
    f.sampleRate = rate;
}

}  // namespace amq::analysis
