#include "amq/engine/engine.hpp"

#include "amq/params/mappings.hpp"

#include <algorithm>
#include <cmath>

namespace amq::engine {

namespace {

using params::BandField;
using params::GlobalParam;

// PROVISIONAL ramp policies: the DSP lead sets them from zipper measurements (area 09 open
// question). Units are the plain units of each field.
RampPolicy policyFor(BandField f) noexcept {
    switch (f) {
        case BandField::Freq: return {20.0, 5.0, 2.0};         // log2 Hz: 2 octaves is a jump
        case BandField::Gain: return {20.0, 5.0, 18.0};        // dB
        case BandField::Q: return {20.0, 5.0, 0.25};           // Q position n
        case BandField::DynRange: return {20.0, 5.0, 18.0};    // dB
        case BandField::Threshold: return {20.0, 5.0, 24.0};   // dB
        default: return {};
    }
}

constexpr double kDistanceFloorDb = -120.0;  // level distances are measured above this floor
constexpr int kSlotFreq = 0, kSlotGain = 1, kSlotQ = 2;

double levelDbForDistance(double db) noexcept { return std::max(db, kDistanceFloorDb); }

}  // namespace

Engine::Engine(const params::ParameterTable& table) noexcept : table_(table) {
    for (int b = 0; b < params::kBandCount; ++b)
        for (size_t k = 0; k < kRampedBandFields.size(); ++k) {
            auto& f = field(b, static_cast<int>(k));
            f.index = params::bandParam(b, kRampedBandFields[k]);
            f.policy = policyFor(kRampedBandFields[k]);
        }
    gainScalePolicy_ = {20.0, 5.0, 100.0};  // percent
    levelPolicy_ = {20.0, 5.0, 24.0};       // dB distance
    panPolicy_ = {20.0, 5.0, 100.0};        // pan units
    polarityPolicy_ = {10.0, 10.0, 2.0, true};
    wetPolicy_ = {kEnableFadeMs, kEnableFadeMs, 2.0, true};
    snapToTable();
    pendingSnap_ = false;
}

int Engine::rampSlot(BandField f) noexcept {
    for (size_t k = 0; k < kRampedBandFields.size(); ++k)
        if (kRampedBandFields[k] == f) return static_cast<int>(k);
    return -1;
}

bool Engine::prepare(double sampleRate, uint32_t maxFrames) noexcept {
    prepared_ = false;
    if (!(sampleRate >= kMinSampleRate - 1.0 && sampleRate <= kMaxSampleRate + 1.0) || maxFrames == 0) {
        sampleRate_ = 48000.0;
        return false;
    }
    try {
        work_.assign(static_cast<size_t>(kMaxChannels) * maxFrames, 0.0);
    } catch (...) {
        return false;
    }
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    swapDelaySamples_ = static_cast<uint64_t>(std::llround(kRefineSwapDelayMs * 1e-3 * sampleRate));
    refineGlideSamples_ = static_cast<uint32_t>(std::llround(kRefineGlideMs * 1e-3 * sampleRate));
    pos_ = rampClock_ = 0;
    stats_ = {};
    for (auto& b : bands_) {
        b = Band{};
    }
    snapToTable();  // seed from current values, never from defaults
    readBands(0, true, kMaxChannels, true);
    if (mode_ == DesignMode::Thread) worker_.start();
    rampClock_ = 0;  // ramps are snapped, so nothing moved; sample 0 is read again in process()
    // The channel layout is known only in process(): its first read snaps each band on or off
    // for the real layout (a Left band must not fade out of a mono host's only channel).
    pendingSnap_ = true;
    prepared_ = true;
    return true;
}

void Engine::release() noexcept {
    prepared_ = false;
    worker_.stop();
}

void Engine::resetBandStates() noexcept {
    for (auto& b : bands_)
        for (auto& ch : b.state) ch.fill({});
}

void Engine::snapToTable() noexcept {
    for (auto& f : fields_) f.ramp.snap(table_.value(f.index));
    gainScale_.snap(table_.value(params::globalParam(GlobalParam::GainScale)));
    levelTargetDb_ = table_.value(params::globalParam(GlobalParam::OutputLevel));
    levelGain_.snap(params::outputLevelGain(levelTargetDb_));
    pan_.snap(table_.value(params::globalParam(GlobalParam::OutputPan)));
    polarity_.snap(table_.value(params::globalParam(GlobalParam::Invert)) >= 0.5 ? -1.0 : 1.0);
    for (auto& b : bands_) b.event = true;
    pendingSnap_ = true;
}

void Engine::advanceBandRampsTo(uint64_t sample) noexcept {
    if (sample <= rampClock_) return;
    const uint64_t n = sample - rampClock_;
    for (auto& f : fields_) f.ramp.advance(n);
    gainScale_.advance(n);
    rampClock_ = sample;
}

void Engine::retarget(Field& f, double target) noexcept {
    if (target == f.ramp.target()) return;
    f.ramp.retarget(target, rampSamples(f.policy, target - f.ramp.current(), sampleRate_));
}

void Engine::retargetAll() noexcept {
    const double gs = table_.value(params::globalParam(GlobalParam::GainScale));
    const bool gsChanged = gs != gainScale_.target();
    if (gsChanged) gainScale_.retarget(gs, rampSamples(gainScalePolicy_, gs - gainScale_.current(), sampleRate_));

    for (int b = 0; b < params::kBandCount; ++b) {
        Band& band = bands_[static_cast<size_t>(b)];
        bool changed = gsChanged;
        for (int k = 0; k < static_cast<int>(kRampedBandFields.size()); ++k) {
            Field& f = field(b, k);
            const double t = table_.value(f.index);
            if (t != f.ramp.target()) {
                retarget(f, t);
                changed = true;
            }
        }
        const std::array<double, 5> sig = {table_.value(params::bandParam(b, BandField::Used)),
                                           table_.value(params::bandParam(b, BandField::Enabled)),
                                           table_.value(params::bandParam(b, BandField::Placement)),
                                           table_.value(params::bandParam(b, BandField::Shape)),
                                           table_.value(params::bandParam(b, BandField::Slope))};
        if (sig != band.seen) {
            band.seen = sig;
            changed = true;
        }
        band.event = band.event || changed;
    }

    const double db = table_.value(params::globalParam(GlobalParam::OutputLevel));
    if (db != levelTargetDb_) {
        // Time from the distance in dB between the current gain and the new level.
        const double curDb = levelGain_.current() > 0.0 ? 20.0 * std::log10(levelGain_.current()) : kDistanceFloorDb;
        const uint32_t n = rampSamples(levelPolicy_, levelDbForDistance(db) - levelDbForDistance(curDb), sampleRate_);
        levelTargetDb_ = db;
        levelGain_.retarget(params::outputLevelGain(db), n);
    }

    const double pan = table_.value(params::globalParam(GlobalParam::OutputPan));
    if (pan != pan_.target()) pan_.retarget(pan, rampSamples(panPolicy_, pan - pan_.current(), sampleRate_));

    const double pol = table_.value(params::globalParam(GlobalParam::Invert)) >= 0.5 ? -1.0 : 1.0;
    if (pol != polarity_.target())
        polarity_.retarget(pol, rampSamples(polarityPolicy_, pol - polarity_.current(), sampleRate_));
}

void Engine::designBand(int b, uint64_t sample, double freqLog2, double gainDb, double qPos, int shape,
                        int slope, bool hardSwitch) noexcept {
    Band& band = bands_[static_cast<size_t>(b)];
    const int oldShape = band.dShape, oldSlope = band.dSlope;
    dsp::BandSpec spec;
    spec.shape = static_cast<dsp::Shape>(shape);
    spec.slope = static_cast<dsp::Slope>(slope);
    spec.frequencyHz = std::exp2(freqLog2);
    spec.gainDb = gainDb;
    spec.q = params::qFromPosition(qPos);
    dsp::SvfCascade next;
    const auto res = dsp::designBandRealtime(spec, sampleRate_, next);
    ++stats_.designs;
    band.dFreq = freqLog2;
    band.dGain = gainDb;
    band.dQ = qPos;
    band.dShape = shape;
    band.dSlope = slope;
    band.lastDesignAt = sample;
    band.designed = true;
    if (res == dsp::RealtimeDesignResult::Unsupported) {
        ++stats_.unsupported;
        band.runnable = false;  // keeps the last coefficients only for a fade-out
        return;
    }
    if (res == dsp::RealtimeDesignResult::BilinearFallback) ++stats_.bilinearFallbacks;
    const bool sameStructure = band.runnable && !hardSwitch && oldShape == shape && oldSlope == slope;
    if (sameStructure) {
        // Glide over one redesign interval. Leaving a refined design (which may have an extra
        // correction section) pairs sections by proximity and pads with identity sections.
        startGlide(band, next, redesignInterval_, band.refined || band.cur.sections.size() != next.sections.size());
    } else {
        // Switch at once (activation, restore, shape or slope change).
        // PROVISIONAL: a shape or slope change is not crossfaded yet (area 01 R17, Phase 2).
        const size_t n = next.sections.size();
        const size_t old = band.runnable ? band.cur.sections.size() : 0;
        for (auto& ch : band.state)
            for (size_t i = hardSwitch ? 0 : old; i < ch.size(); ++i) ch[i] = {};
        band.cur = band.goal = next;
        band.glideLeft = 0;
        band.coeffs.resize(n);
        for (size_t k = 0; k < n; ++k) band.coeffs[k] = dsp::svfCoeffs(next.sections[k]);
    }
    band.runnable = true;
    band.refined = false;  // a fast design replaces any refined one; refine again once settled
    band.refineDone = false;
    band.pendingId = 0;
}

namespace {

bool isIdentity(const dsp::SvfParams& p) noexcept { return p.m0 == 1.0 && p.m1 == 0.0 && p.m2 == 0.0; }

double sectionDistance(const dsp::SvfParams& a, const dsp::SvfParams& b) noexcept {
    return std::abs(std::log(a.g / b.g)) + std::abs(std::log(a.k / b.k));
}

}  // namespace

void Engine::startGlide(Band& band, const dsp::SvfCascade& next, uint32_t samples, bool byProximity) noexcept {
    const size_t nc = band.cur.sections.size(), nn = next.sections.size();
    const size_t m = std::max(nc, nn);
    dsp::SvfCascade target;
    target.sections.resize(m);
    // Pad the current cascade with identity sections (output = input exactly, any g and k).
    band.cur.sections.resize(m);
    for (size_t i = nc; i < m; ++i) {
        band.cur.sections[i] = dsp::SvfParams{1.0, 2.0, 1.0, 0.0, 0.0};
        for (auto& ch : band.state) ch[i] = {};
    }
    if (!byProximity && nc == nn) {
        for (size_t i = 0; i < m; ++i) target.sections[i] = next.sections[i];
    } else {
        // Pair each current section with the nearest unused new one (log g, log k), so the
        // glide moves poles a short way. Leftover current sections glide to identity; leftover
        // new sections take over the identity pads, whose g and k start at the target's.
        bool taken[dsp::kMaxDigitalSections] = {};
        for (size_t i = 0; i < nc; ++i) {
            int best = -1;
            double bestD = 0.0;
            for (size_t j = 0; j < nn; ++j) {
                if (taken[j]) continue;
                const double d = sectionDistance(band.cur.sections[i], next.sections[j]);
                if (best < 0 || d < bestD) best = static_cast<int>(j), bestD = d;
            }
            if (best >= 0) {
                taken[best] = true;
                target.sections[i] = next.sections[static_cast<size_t>(best)];
            } else {
                const auto& c = band.cur.sections[i];
                target.sections[i] = dsp::SvfParams{c.g, c.k, 1.0, 0.0, 0.0};
            }
        }
        size_t pad = nc;
        for (size_t j = 0; j < nn && pad < m; ++j) {
            if (taken[j]) continue;
            target.sections[pad] = next.sections[j];
            band.cur.sections[pad].g = next.sections[j].g;
            band.cur.sections[pad].k = next.sections[j].k;
            ++pad;
        }
    }
    band.goal = target;
    band.step.sections.resize(m);
    const double inv = 1.0 / static_cast<double>(samples ? samples : 1);
    for (size_t k = 0; k < m; ++k) {
        const auto& a = band.cur.sections[k];
        const auto& z = target.sections[k];
        band.step.sections[k] = {(z.g - a.g) * inv, (z.k - a.k) * inv, (z.m0 - a.m0) * inv, (z.m1 - a.m1) * inv,
                                 (z.m2 - a.m2) * inv};
    }
    band.coeffs.resize(m);
    for (size_t k = 0; k < m; ++k) band.coeffs[k] = dsp::svfCoeffs(band.cur.sections[k]);
    band.glideLeft = samples ? samples : 1;
}

void Engine::dropIdentitySections(Band& band) noexcept {
    // Identity sections pass audio unchanged, so removing them is exact.
    size_t w = 0;
    const size_t n = band.cur.sections.size();
    for (size_t r = 0; r < n; ++r) {
        if (isIdentity(band.cur.sections[r]) && n > 1) continue;
        if (w != r) {
            band.cur.sections[w] = band.cur.sections[r];
            band.coeffs[w] = band.coeffs[r];
            for (auto& ch : band.state) ch[w] = ch[r];
        }
        ++w;
    }
    if (w == 0) {  // everything was identity: keep one so the cascade is never empty
        w = 1;
    }
    band.cur.sections.resize(w);
    band.goal = band.cur;
    band.coeffs.resize(w);
}

void Engine::refineStep(int b, uint64_t sample) noexcept {
    Band& band = bands_[static_cast<size_t>(b)];
    if (!band.runnable || band.refineDone) return;
    if (band.dShape == static_cast<int>(dsp::Shape::AllPass)) {  // its bilinear design is already final
        band.refineDone = true;
        return;
    }
    if (band.pendingId == 0) {
        const DesignInputs in{sampleRate_, band.dFreq, band.dGain, band.dQ, band.dShape, band.dSlope};
        // Settled: no ramp of this band (or Gain Scale) is moving.
        for (int k = 0; k <= kSlotQ; ++k)
            if (field(b, k).ramp.active()) return;
        if (gainScale_.active()) return;
        if (offline_.load(std::memory_order_relaxed)) {
            dsp::SvfCascade c;  // offline render: allocation and milliseconds are fine here
            if (designRefinedCascade(in, c)) {
                startGlide(band, c, 1, true);
                band.refined = true;
                ++stats_.refinedSwaps;
            } else {
                ++stats_.refineFailures;
            }
            band.refineDone = true;
            return;
        }
        band.pendingId = ++nextRequestId_;
        band.requestedAt = sample;
        worker_.post(b, DesignRequest{band.pendingId, in});
        ++stats_.refineRequests;
        return;
    }
    // Swap at a fixed delay after the request, so normal playback is repeatable. Poll on the
    // redesign grid only.
    if (sample < band.requestedAt + swapDelaySamples_ || (sample - band.requestedAt) % redesignInterval_ != 0 ||
        worker_.readyId(b) != band.pendingId)
        return;  // not due or not ready
    DesignResult r;
    if (!worker_.tryResult(b, r) || r.id != band.pendingId) return;  // not ready yet
    band.pendingId = 0;
    band.refineDone = true;
    if (!r.ok) {
        ++stats_.refineFailures;
        return;
    }
    startGlide(band, r.cascade, refineGlideSamples_, true);
    band.refined = true;
    ++stats_.refinedSwaps;
}

void Engine::readBands(uint64_t sample, bool pieceStart, uint32_t numOut, bool snapWet) noexcept {
    const bool onGrid = sample % readInterval_ == 0;
    advanceBandRampsTo(sample + 1);  // ramps now hold the value of this sample
    // Designs target the values one redesign interval ahead; the coefficients glide there.
    const uint64_t ahead = redesignInterval_;
    const double scale = gainScale_.valueAhead(ahead) / 100.0;
    for (int b = 0; b < params::kBandCount; ++b) {
        Band& band = bands_[static_cast<size_t>(b)];
        const bool eventNow = pieceStart && band.event;
        if (!onGrid && !eventNow && !snapWet) continue;
        if (pieceStart) band.event = false;

        const bool used = table_.value(params::bandParam(b, BandField::Used)) >= 0.5;
        const bool enabled = table_.value(params::bandParam(b, BandField::Enabled)) >= 0.5;
        const int placement = static_cast<int>(table_.value(params::bandParam(b, BandField::Placement)));
        const int shape = static_cast<int>(table_.value(params::bandParam(b, BandField::Shape)));
        const int slope = static_cast<int>(table_.value(params::bandParam(b, BandField::Slope)));
        // Placement (area 02 R5). Left and Mid run on the first channel of a pair, Right and Side
        // on the second; Mid and Side run in the M/S stage. On a mono channel only Stereo bands run.
        uint8_t mask = 0, stage = 0;
        switch (placement) {
            case 0: mask = numOut >= 2 ? 3 : 1; break;              // Stereo
            case 1: mask = numOut >= 2 ? 1 : 0; break;              // Left
            case 2: mask = numOut >= 2 ? 2 : 0; break;              // Right
            case 3: mask = numOut >= 2 ? 1 : 0, stage = 1; break;  // Mid
            case 4: mask = numOut >= 2 ? 2 : 0, stage = 1; break;  // Side
            default: break;
        }
        const bool active = used && enabled && mask != 0;

        if (active) {
            const bool off = band.wet.current() == 0.0 && !band.wet.active();
            if (off) {  // switching on from silence: clean state, design now
                for (auto& ch : band.state) ch.fill({});
                band.mask = mask;
                band.stage = stage;
            } else if (stage != band.stage) {
                // PROVISIONAL: moving a sounding band between the L/R and M/S stages restarts its
                // state (no crossfade yet).
                for (auto& ch : band.state) ch.fill({});
                band.mask = mask;
                band.stage = stage;
            } else if (mask != band.mask) {
                for (uint32_t c = 0; c < kMaxChannels; ++c)
                    if ((mask & (1u << c)) && !(band.mask & (1u << c))) band.state[c].fill({});
                band.mask = mask;
            }
            const double f = field(b, kSlotFreq).ramp.valueAhead(ahead);
            const double g = std::clamp(field(b, kSlotGain).ramp.valueAhead(ahead) * scale, -30.0, 30.0);  // R13
            const double q = field(b, kSlotQ).ramp.valueAhead(ahead);
            const bool changed = !band.designed || f != band.dFreq || g != band.dGain || q != band.dQ ||
                                 shape != band.dShape || slope != band.dSlope;
            if (changed) {
                const bool allowed = !band.designed || off || eventNow || snapWet ||
                                     sample - band.lastDesignAt >= redesignInterval_ ||
                                     std::abs(f - band.dFreq) >= kJumpOctaves || shape != band.dShape ||
                                     slope != band.dSlope;
                if (allowed) designBand(b, sample, f, g, q, shape, slope, off || snapWet);
            } else {
                refineStep(b, sample);
            }
        }
        const double target = (active && band.runnable) ? 1.0 : 0.0;
        if (snapWet) band.wet.snap(target);
        else if (target != band.wet.target()) band.wet.retarget(target, rampSamples(wetPolicy_, 1.0, sampleRate_));
    }
}

void Engine::runStage(uint8_t stage, double* const* w, uint32_t outCh, uint32_t off, uint32_t n) noexcept {
    for (auto& band : bands_) {
        if (band.stage != stage) continue;
        if (!band.wet.active() && band.wet.current() == 0.0) continue;  // off: bit-transparent
        for (uint32_t i = 0; i < n; ++i) {
            if (band.glideLeft) {
                const size_t m = band.coeffs.size();
                if (--band.glideLeft == 0) band.cur = band.goal;  // land exactly on the design
                for (size_t k = 0; k < m; ++k) {
                    auto& c = band.cur.sections[k];
                    if (band.glideLeft) {
                        const auto& d = band.step.sections[k];
                        c.g += d.g, c.k += d.k, c.m0 += d.m0, c.m1 += d.m1, c.m2 += d.m2;
                    }
                    band.coeffs[k] = dsp::svfCoeffs(c);
                }
                if (band.glideLeft == 0) dropIdentitySections(band);
            }
            const size_t ns = band.coeffs.size();
            const double wet = band.wet.next();
            for (uint32_t c = 0; c < outCh; ++c) {
                if (!(band.mask & (1u << c))) continue;
                const double x = w[c][off + i];
                double y = x;
                auto& st = band.state[c];
                for (size_t k = 0; k < ns; ++k) y = dsp::svfTick(band.coeffs[k], st[k], y);
                w[c][off + i] = wet == 1.0 ? y : x + wet * (y - x);
            }
        }
    }
}

void Engine::process(const float* const* in, uint32_t numIn, float* const* out, uint32_t numOut,
                     uint32_t frames) noexcept {
    if (!out || frames == 0) return;
    const bool haveInput = in && numIn > 0;
    if (!prepared_ || !haveInput) {  // safe silence (area 02 R3)
        for (uint32_t c = 0; c < numOut; ++c)
            if (out[c]) std::fill(out[c], out[c] + frames, 0.0f);
        return;
    }
    if (frames > maxFrames_) {  // hosts keep to max_frames; split defensively if not
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t n = std::min(maxFrames_, frames - done);
            const float* ip[kMaxChannels] = {};
            float* op[kMaxChannels] = {};
            for (uint32_t c = 0; c < std::min(numIn, kMaxChannels); ++c) ip[c] = in[c] + done;
            for (uint32_t c = 0; c < std::min(numOut, kMaxChannels); ++c) op[c] = out[c] + done;
            process(ip, std::min(numIn, kMaxChannels), op, std::min(numOut, kMaxChannels), n);
            done += n;
        }
        for (uint32_t c = kMaxChannels; c < numOut; ++c)
            if (out[c]) std::fill(out[c], out[c] + frames, 0.0f);
        return;
    }

    advanceBandRampsTo(pos_);
    retargetAll();

    const uint32_t outCh = std::min(numOut, kMaxChannels);
    double* w[kMaxChannels] = {work_.data(), work_.data() + maxFrames_};
    for (uint32_t c = 0; c < outCh; ++c) {
        const float* x = in[std::min(c, numIn - 1)];
        for (uint32_t i = 0; i < frames; ++i) w[c][i] = x[i];
    }

    // Bands in series, in index order (area 01 R7), sub-block by sub-block between reads.
    const uint64_t end = pos_ + frames;
    uint64_t s = pos_;
    bool first = true;
    const bool snap = pendingSnap_;
    pendingSnap_ = false;
    while (s < end) {
        if (first || s % readInterval_ == 0) readBands(s, first, numOut, first && snap);
        first = false;
        const uint64_t subEnd = std::min(end, (s / readInterval_ + 1) * readInterval_);
        const uint32_t off = static_cast<uint32_t>(s - pos_);
        const uint32_t n = static_cast<uint32_t>(subEnd - s);
        runStage(0, w, outCh, off, n);
        // The M/S stage, with encode and decode inserted only while a Mid or Side band is
        // audible (area 03 R3). The round trip is exact to rounding, so insertion cannot click.
        bool msAudible = false;
        if (outCh >= 2)
            for (const auto& band : bands_)
                if (band.stage == 1 && (band.wet.active() || band.wet.current() != 0.0)) {
                    msAudible = true;
                    break;
                }
        if (msAudible) {
            dsp::midSide(dsp::MidSideMode::Encode, w[0] + off, w[1] + off, n);
            runStage(1, w, outCh, off, n);
            dsp::midSide(dsp::MidSideMode::Decode, w[0] + off, w[1] + off, n);
        }
        s = subEnd;
    }

    // Level / Pan / Invert node (area 03 R10-R12). Pan is a linear balance that only
    // attenuates, in stereo L/R only; the M/S pan mode arrives with area 04.
    const bool stereo = outCh >= 2;
    const bool panLR = stereo && table_.value(params::globalParam(GlobalParam::PanMode)) < 0.5;
    for (uint32_t i = 0; i < frames; ++i) {
        const double g = levelGain_.next() * polarity_.next();
        const double p = pan_.next();
        double gl = g, gr = g;
        if (panLR) {
            if (p > 0.0) gl *= 1.0 - p / 100.0;
            else if (p < 0.0) gr *= 1.0 + p / 100.0;
        }
        out[0][i] = static_cast<float>(w[0][i] * gl);
        if (stereo) out[1][i] = static_cast<float>(w[1][i] * gr);
    }
    for (uint32_t c = kMaxChannels; c < numOut; ++c)
        if (out[c]) std::fill(out[c], out[c] + frames, 0.0f);
    pos_ = end;
}

double Engine::bandValue(int band, BandField f) const noexcept {
    const int k = rampSlot(f);
    if (band < 0 || band >= params::kBandCount || k < 0) return 0.0;
    return field(band, k).ramp.current();
}

}  // namespace amq::engine
