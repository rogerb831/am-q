#include "amq/engine/engine.hpp"

#include "amq/engine/band_response.hpp"

#include "amq/params/mappings.hpp"

#include <algorithm>
#include <cmath>

static_assert(amq::analysis::kBandCount == amq::params::kBandCount, "snapshot band count");

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
    bypassPolicy_ = {kBypassFadeMs, kBypassFadeMs, 2.0, true};
    fadePolicy_ = {kModeFadeMs, kModeFadeMs, 2.0, true};
    soloPolicy_ = {kSoloFadeMs, kSoloFadeMs, 2.0, true};
    soloGainPolicy_ = {20.0, 5.0, 40.0};  // dB
    fade_.snap(1.0);
    for (auto& b : bands_) b.xfade.snap(1.0);
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
    analyzer_.stop();  // it writes publisher_'s spectra; nothing may run while we prepare
    try {
        work_.assign(static_cast<size_t>(kMaxChannels) * maxFrames, 0.0);
        solo_.assign(static_cast<size_t>(kMaxChannels) * maxFrames, 0.0);
        soloMixBuf_.assign(maxFrames, 0.0);
        if (!analyzerSubscribed_) {
            analyzer_.subscribe(&display_, analysis::kAllSources, analysis::SizeChoice::Display);
            analyzerSubscribed_ = true;
        }
        if (!analyzer_.prepare(sampleRate)) return false;
        display_.prepare(&publisher_, sampleRate, analyzer_.fftSize(analysis::SizeChoice::Display));
    } catch (...) {
        return false;
    }
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    swapDelaySamples_ = static_cast<uint64_t>(std::llround(kRefineSwapDelayMs * 1e-3 * sampleRate));
    refineGlideSamples_ = static_cast<uint32_t>(std::llround(kRefineGlideMs * 1e-3 * sampleRate));
    pos_ = rampClock_ = 0;
    stats_ = {};
    publisher_.prepare(sampleRate, kMaxChannels);  // dynamics arrive with area 06: gain change stays 0 dB
    for (auto& b : bands_) {
        b = Band{};
    }
    snapToTable();  // seed from current values, never from defaults
    readBands(0, true, kMaxChannels, true);
    // The processing mode is latched here: its latency is what activation reports (area 02 R9).
    activeMode_ = requestedMode();
    npDirty_ = npPosted_ = false;
    npLastPost_ = 0;
    npChannels_ = kMaxChannels;
    npInterval_ = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(kKernelIntervalMs * 1e-3 * sampleRate)));
    if (activeMode_ == dsp::ProcessingMode::NaturalPhase) {
        fillCorrectionRequest(kMaxChannels);
        npRequest_.id = ++npNextId_;
        if (!np_.prepare(maxFrames, npRequest_)) return false;
    }
    // The bypass dry path is delayed by exactly the latency this activation reports (area 02 R2).
    dryLen_ = dsp::latencySamples(activeMode_, 0, sampleRate, false);
    dryPos_ = 0;
    try {
        dryLine_.assign(static_cast<size_t>(kMaxChannels) * dryLen_, 0.0f);
        dry_.assign(static_cast<size_t>(kMaxChannels) * maxFrames, 0.0);
    } catch (...) {
        return false;
    }
    // The audition starts clean; a band soloed already is heard at once (snap in the first piece).
    soloBand_ = -1;
    soloMix_.snap(0.0);
    soloState_ = {};
    soloShape_ = -1;
    // After a fade-out for a mode change, fade back in; any other activation starts at full level.
    silentSamples_ = 0;
    gaveUpMode_ = -1;
    if (fadedOut_.exchange(false)) {
        fade_.snap(0.0);
        fade_.retarget(1.0, rampSamples(fadePolicy_, 1.0, sampleRate));
    } else {
        fade_.snap(1.0);
    }
    if (mode_ == DesignMode::Thread) {
        worker_.start();
        analyzer_.start();
        if (activeMode_ == dsp::ProcessingMode::NaturalPhase) np_.startWorker();
    }
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
    analyzer_.stop();
    np_.stopWorker();
}

dsp::ProcessingMode Engine::requestedMode() const noexcept {
    // Linear Phase is Phase 2: until then it runs, and reports, as Zero Latency.
    const int m = static_cast<int>(table_.value(params::globalParam(GlobalParam::Mode)));
    return m == static_cast<int>(dsp::ProcessingMode::NaturalPhase) ? dsp::ProcessingMode::NaturalPhase
                                                                     : dsp::ProcessingMode::ZeroLatency;
}

void Engine::fillCorrectionRequest(uint32_t channels) noexcept {
    npRequest_.sampleRate = sampleRate_;
    npRequest_.channels = channels;
    for (size_t b = 0; b < bands_.size(); ++b) {
        const Band& band = bands_[b];
        auto& cb = npRequest_.bands[b];
        cb.active = band.runnable && band.mask != 0 && band.wet.target() == 1.0;
        cb.stage = band.stage;
        cb.mask = band.mask;
        cb.in = DesignInputs{sampleRate_, band.dFreq, band.dGain, band.dQ, band.dShape, band.dSlope};
        cb.cascade = band.goal;  // where the running glide lands: the exact sections (area 04 R10)
    }
}

void Engine::scheduleKernel(uint64_t sample, uint32_t channels) noexcept {
    if (channels != npChannels_) {
        npChannels_ = channels;
        npDirty_ = true;
    }
    if (!npDirty_) return;
    if (npPosted_ && sample < npLastPost_ + npInterval_) return;  // rate limit (area 04 R13)
    fillCorrectionRequest(channels);
    npRequest_.id = ++npNextId_;
    if (offline_.load(std::memory_order_relaxed)) np_.buildNow(npRequest_);
    else np_.post(npRequest_);
    npDirty_ = false;
    npPosted_ = true;
    npLastPost_ = sample;
    ++stats_.kernelRequests;
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
    panMode_.snap(table_.value(params::globalParam(GlobalParam::PanMode)) >= 0.5 ? 1.0 : 0.0);
    polarity_.snap(table_.value(params::globalParam(GlobalParam::Invert)) >= 0.5 ? -1.0 : 1.0);
    soloGainDb_.snap(table_.value(params::globalParam(GlobalParam::SoloGain)));
    bypass_.snap(table_.value(params::globalParam(GlobalParam::Bypass)) >= 0.5 ? 1.0 : 0.0);
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
    // Pan Mode: the old and new output graphs crossfade linearly over kPanModeFadeMs (area 04 R4).
    // Both graphs are linear gains on the same input, so running both and mixing is the same as
    // mixing their 2x2 matrices. A switch back midway reverses from the current mix.
    const double pm = table_.value(params::globalParam(GlobalParam::PanMode)) >= 0.5 ? 1.0 : 0.0;
    if (pm != panMode_.target()) {
        const double len = std::abs(pm - panMode_.current()) * kPanModeFadeMs * 1e-3 * sampleRate_;
        panMode_.retarget(pm, static_cast<uint32_t>(std::lround(len)));
    }

    const double sg = table_.value(params::globalParam(GlobalParam::SoloGain));
    if (sg != soloGainDb_.target())
        soloGainDb_.retarget(sg, rampSamples(soloGainPolicy_, sg - soloGainDb_.current(), sampleRate_));

    const double pol = table_.value(params::globalParam(GlobalParam::Invert)) >= 0.5 ? -1.0 : 1.0;
    const double byp = table_.value(params::globalParam(GlobalParam::Bypass)) >= 0.5 ? 1.0 : 0.0;
    if (byp != bypass_.target()) bypass_.retarget(byp, rampSamples(bypassPolicy_, 1.0, sampleRate_));

    // Fade out while the table asks for another mode, back in if it stops asking (area 02 R13).
    const bool waiting = restartNeeded() && static_cast<int>(requestedMode()) != gaveUpMode_;
    const double fadeTarget = waiting ? 0.0 : 1.0;
    if (fadeTarget != fade_.target()) fade_.retarget(fadeTarget, rampSamples(fadePolicy_, 1.0, sampleRate_));

    if (pol != polarity_.target())
        polarity_.retarget(pol, rampSamples(polarityPolicy_, pol - polarity_.current(), sampleRate_));
}

void Engine::designBand(int b, uint64_t sample, double freqLog2, double gainDb, double qPos, int shape,
                        int slope, bool hardSwitch) noexcept {
    Band& band = bands_[static_cast<size_t>(b)];
    const int oldShape = band.dShape, oldSlope = band.dSlope;
    npDirty_ = true;  // the Natural Phase correction follows every design
    dsp::SvfCascade next;
    const auto res = dsp::designBandRealtime(bandSpecFrom({sampleRate_, freqLog2, gainDb, qPos, shape, slope}),
                                             sampleRate_, next);
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
        // A shape or slope change on a sounding band crossfades the old and new cascades over
        // kStructureFadeMs (R17): the old one keeps its coefficients and state and fades out,
        // the new one starts from rest and fades in. Activation and restore switch at once.
        const size_t n = next.sections.size();
        if (band.runnable && !hardSwitch) {
            band.oldCoeffs = band.coeffs;
            band.oldState = band.state;
            band.xfade.snap(0.0);
            band.xfade.retarget(1.0, static_cast<uint32_t>(std::lround(kStructureFadeMs * 1e-3 * sampleRate_)));
            ++stats_.structureFades;
        } else {
            band.xfade.snap(1.0);
        }
        for (auto& ch : band.state) ch.fill({});
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
    npDirty_ = true;
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
            const bool structure = shape != band.dShape || slope != band.dSlope;
            const bool changed = !band.designed || f != band.dFreq || g != band.dGain || q != band.dQ || structure;
            // A further shape or slope change waits for a running structure crossfade to end.
            const bool waiting = structure && band.designed && !off && !snapWet && band.xfade.active();
            if (changed && !waiting) {
                const bool allowed = !band.designed || off || eventNow || snapWet ||
                                     sample - band.lastDesignAt >= redesignInterval_ ||
                                     std::abs(f - band.dFreq) >= kJumpOctaves || shape != band.dShape ||
                                     slope != band.dSlope;
                if (allowed) designBand(b, sample, f, g, q, shape, slope, off || snapWet);
            } else {
                refineStep(b, sample);
            }
        }
        // Near-zero bands (R16): a gain shape whose effective gain rounds to 0 dB fades to bypass
        // and is then bit-transparent. The decision uses the gain the design targets.
        bool nearZero = false;
        if (active && band.designed && dsp::shapeHasGain(static_cast<dsp::Shape>(band.dShape)))
            nearZero = std::abs(band.dGain) < kNearZeroGainDb;
        const double target = (active && band.runnable && !nearZero) ? 1.0 : 0.0;
        if (snapWet) band.wet.snap(target);
        else if (target != band.wet.target()) band.wet.retarget(target, rampSamples(wetPolicy_, 1.0, sampleRate_));
        const auto seen = static_cast<uint8_t>((target == 1.0 ? 1u : 0u) | (band.mask << 1) | (band.stage << 3));
        if (seen != npSeen_[static_cast<size_t>(b)]) {
            npSeen_[static_cast<size_t>(b)] = seen;
            npDirty_ = true;
        }
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
            const bool fading = band.xfade.active();
            const double a = fading ? band.xfade.next() : 1.0;
            for (uint32_t c = 0; c < outCh; ++c) {
                if (!(band.mask & (1u << c))) continue;
                const double x = w[c][off + i];
                double y = x;
                auto& st = band.state[c];
                for (size_t k = 0; k < ns; ++k) y = dsp::svfTick(band.coeffs[k], st[k], y);
                if (fading) {
                    double yo = x;
                    auto& so = band.oldState[c];
                    for (size_t k = 0; k < band.oldCoeffs.size(); ++k) yo = dsp::svfTick(band.oldCoeffs[k], so[k], yo);
                    y = yo + a * (y - yo);
                }
                w[c][off + i] = wet == 1.0 ? y : x + wet * (y - x);
            }
        }
    }
}

void Engine::process(const float* const* in, uint32_t numIn, float* const* out, uint32_t numOut,
                     uint32_t frames, const float* const* sc, uint32_t numSc) noexcept {
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
            const float* sp[kMaxChannels] = {};
            float* op[kMaxChannels] = {};
            const uint32_t ns = sc ? std::min(numSc, kMaxChannels) : 0;
            for (uint32_t c = 0; c < std::min(numIn, kMaxChannels); ++c) ip[c] = in[c] + done;
            for (uint32_t c = 0; c < ns; ++c) sp[c] = sc[c] ? sc[c] + done : nullptr;
            for (uint32_t c = 0; c < std::min(numOut, kMaxChannels); ++c) op[c] = out[c] + done;
            process(ip, std::min(numIn, kMaxChannels), op, std::min(numOut, kMaxChannels), n, ns ? sp : nullptr, ns);
            done += n;
        }
        for (uint32_t c = kMaxChannels; c < numOut; ++c)
            if (out[c]) std::fill(out[c], out[c] + frames, 0.0f);
        return;
    }

    advanceBandRampsTo(pos_);
    retargetAll();

    const uint32_t outCh = std::min(numOut, kMaxChannels);
    publisher_.meterInput(in, std::min(numIn, kMaxChannels), frames);
    analyzer_.write(analysis::Source::Pre, in, std::min(numIn, kMaxChannels), frames);  // before any output: in may alias out
    if (sc && numSc > 0 && sc[0]) {
        publisher_.meterSidechain(sc, std::min(numSc, kMaxChannels), frames);
        analyzer_.write(analysis::Source::External, sc, std::min(numSc, kMaxChannels), frames);
    }
    double* w[kMaxChannels] = {work_.data(), work_.data() + maxFrames_};
    double* dry[kMaxChannels] = {dry_.data(), dry_.data() + maxFrames_};
    for (uint32_t c = 0; c < outCh; ++c) {
        const float* x = in[std::min(c, numIn - 1)];
        for (uint32_t i = 0; i < frames; ++i) w[c][i] = x[i];
        // The dry path, delayed by the reported latency (area 02 R2). Read before any output
        // is written: in may alias out.
        if (dryLen_ == 0) {
            for (uint32_t i = 0; i < frames; ++i) dry[c][i] = x[i];
        } else {
            float* line = dryLine_.data() + static_cast<size_t>(c) * dryLen_;
            uint32_t p = dryPos_;
            for (uint32_t i = 0; i < frames; ++i) {
                dry[c][i] = line[p];
                line[p] = x[i];
                if (++p == dryLen_) p = 0;
            }
        }
    }
    if (dryLen_) dryPos_ = static_cast<uint32_t>((dryPos_ + frames) % dryLen_);

    // Bands in series, in index order (area 01 R7), sub-block by sub-block between reads.
    const uint64_t end = pos_ + frames;
    uint64_t s = pos_;
    bool first = true;
    const bool snap = pendingSnap_;
    pendingSnap_ = false;
    soloInPiece_ = false;
    while (s < end) {
        if (first || s % readInterval_ == 0) {
            readBands(s, first, numOut, first && snap);
            if (activeMode_ == dsp::ProcessingMode::NaturalPhase) scheduleKernel(s, outCh);
            updateSolo(outCh, first && snap);
        }
        first = false;
        const uint64_t subEnd = std::min(end, (s / readInterval_ + 1) * readInterval_);
        const uint32_t off = static_cast<uint32_t>(s - pos_);
        const uint32_t n = static_cast<uint32_t>(subEnd - s);
        runSolo(dry, outCh, off, n);
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

    // Natural Phase: the combined correction after every IIR stage (area 03 R14).
    if (activeMode_ == dsp::ProcessingMode::NaturalPhase) np_.process(w, outCh, frames);

    // Output graph (area 03 R10-R12, area 04 R1-R5, R8): mode encoder, Level / Pan / Invert,
    // mode decoder. Pan is a linear balance that only attenuates: b1 = 1 - p/100 for p > 0,
    // b2 = 1 + p/100 for p < 0. In L/R mode it scales L by b1 and R by b2. In M/S mode it
    // scales M by b1 and S by b2, so pan right lowers Mid (wider) and pan left lowers Side
    // (mono at full left); with the encode and decode of dsp/mid_side.hpp that is the matrix
    //     L' = (b1+b2)/2 L + (b1-b2)/2 R,   R' = (b1-b2)/2 L + (b1+b2)/2 R.
    // During a Pan Mode switch the two matrices crossfade with a (area 04 R4). Level and Invert
    // scale both components alike, so they are the same in both modes (R5). Mono: no pan (R8).
    const bool stereo = outCh >= 2;
    for (uint32_t i = 0; i < frames; ++i) {
        const double g = levelGain_.next() * polarity_.next();
        const double p = pan_.next();
        const double a = panMode_.next();
        double gl = g, gr = g, x = 0.0;  // x: cross term L -> R and R -> L
        if (stereo) {
            const double b1 = p > 0.0 ? 1.0 - p / 100.0 : 1.0;
            const double b2 = p < 0.0 ? 1.0 + p / 100.0 : 1.0;
            if (a == 0.0) {
                gl *= b1, gr *= b2;
            } else {
                const double h = 0.5 * a * (b2 - b1);  // written so that b1 == b2 stays exact
                gl *= b1 + h, gr *= b2 - h, x = -g * h;
            }
        }
        // Solo audition: crossfade to the audition signal, which has its own level and decode
        // and skips the Level / Pan / Invert node (area 02 R14, area 04 R7).
        // Global bypass: crossfade to the delayed dry input; fully bypassed it is the dry input
        // exactly (area 01 R6, area 02 R2); bypass wins over solo. Then the mode-change fade
        // (area 02 R13).
        const double m = bypass_.next();
        const double fz = fade_.next();
        double yl = w[0][i] * gl, yr = stereo ? w[1][i] * gr : 0.0;
        if (x != 0.0) {
            const double l = w[0][i], r = w[1][i];
            yl += x * r, yr += x * l;
        }
        if (soloInPiece_) {
            const double sm = soloMixBuf_[i];
            if (sm == 1.0) {
                yl = solo_[i];
                if (stereo) yr = solo_[maxFrames_ + i];
            } else if (sm != 0.0) {
                yl += sm * (solo_[i] - yl);
                if (stereo) yr += sm * (solo_[maxFrames_ + i] - yr);
            }
        }
        if (m == 1.0) {
            yl = dry[0][i];
            if (stereo) yr = dry[1][i];
        } else if (m != 0.0) {
            yl += m * (dry[0][i] - yl);
            if (stereo) yr += m * (dry[1][i] - yr);
        }
        if (fz != 1.0) yl *= fz, yr *= fz;
        out[0][i] = static_cast<float>(yl);
        if (stereo) out[1][i] = static_cast<float>(yr);
    }
    const bool silent = fade_.target() == 0.0 && !fade_.active();
    fadedOut_.store(silent, std::memory_order_release);
    if (silent) {
        silentSamples_ += frames;
        if (static_cast<double>(silentSamples_) >= kRestartWaitMs * 1e-3 * sampleRate_) {
            gaveUpMode_ = static_cast<int>(requestedMode());  // the next piece fades back in
            silentSamples_ = 0;
        }
    } else {
        silentSamples_ = 0;
    }
    for (uint32_t c = kMaxChannels; c < numOut; ++c)
        if (out[c]) std::fill(out[c], out[c] + frames, 0.0f);
    publisher_.meterOutput(out, outCh, frames);
    analyzer_.write(analysis::Source::Post, out, outCh, frames);
    publisher_.advance(frames, end);
    pos_ = end;
}

// Solo audition (area 02 R14, R15; area 04 R7). Called at every parameter read. The lowest
// soloed band that is in use is auditioned (PROVISIONAL: one band at a time). Moving the solo to
// another band fades the audition out, switches while silent, and fades back in.
void Engine::updateSolo(uint32_t outCh, bool snap) noexcept {
    int target = -1;
    for (int b = 0; b < params::kBandCount && target < 0; ++b)
        if (table_.value(params::bandParam(b, BandField::Solo)) >= 0.5 &&
            table_.value(params::bandParam(b, BandField::Used)) >= 0.5)
            target = b;

    const bool silent = soloMix_.current() == 0.0 && !soloMix_.active();
    bool fresh = false;
    if (target != soloBand_) {
        if (silent || snap) {
            soloBand_ = target;
            soloState_ = {};
            soloShape_ = -1;
            fresh = true;
            if (target < 0) soloMix_.snap(0.0);
            else if (snap) soloMix_.snap(1.0);
            else soloMix_.retarget(1.0, rampSamples(soloPolicy_, 1.0, sampleRate_));
        } else if (soloMix_.target() != 0.0) {
            soloMix_.retarget(0.0, rampSamples(soloPolicy_, 1.0, sampleRate_));
        }
    } else if (target >= 0 && soloMix_.target() != 1.0) {
        soloMix_.retarget(1.0, rampSamples(soloPolicy_, 1.0, sampleRate_));  // soloed again while fading out
    }
    if (soloBand_ < 0) return;

    // The audition design follows the band's ramped frequency and Q.
    const int b = soloBand_;
    const auto shape = static_cast<dsp::Shape>(static_cast<int>(table_.value(params::bandParam(b, BandField::Shape))));
    const double hz = std::exp2(field(b, rampSlot(BandField::Freq)).ramp.current());
    const double q = dsp::auditionQ(params::qFromPosition(field(b, rampSlot(BandField::Q)).ramp.current()));
    if (hz != soloHz_ || q != soloQ_ || static_cast<int>(shape) != soloShape_) {
        soloHz_ = hz, soloQ_ = q, soloShape_ = static_cast<int>(shape);
        soloCoeffs_ = dsp::svfCoeffs(dsp::auditionDesign(dsp::auditionKind(shape), hz, q, sampleRate_));
    }
    const int placement = static_cast<int>(table_.value(params::bandParam(b, BandField::Placement)));
    soloPlacement_ = static_cast<uint8_t>(outCh >= 2 ? placement : 0);  // mono: the one channel

    // Level: Solo Gain, plus the band's boost for a boosted Bell or shelf (R14). The level moves
    // linearly to each read's value over one read interval, so it never steps.
    double db = soloGainDb_.current();
    if (dsp::auditionIncludesBoost(shape))
        db += std::max(0.0, field(b, rampSlot(BandField::Gain)).ramp.current() * gainScale_.current() / 100.0);
    const double g = std::pow(10.0, db / 20.0);
    if (fresh || snap) soloLevel_.snap(g);
    else if (g != soloLevel_.target()) soloLevel_.retarget(g, readInterval_);
}

// Filters the dry input of samples [off, off + n) into the audition buffer and records the
// mix per sample. Decodes M/S placements to L/R: Mid plays on both sides, Side as +S and -S.
void Engine::runSolo(const double* const* dry, uint32_t outCh, uint32_t off, uint32_t n) noexcept {
    double* mix = soloMixBuf_.data() + off;
    soloGainDb_.advance(n);
    if (soloBand_ < 0) {
        std::fill(mix, mix + n, 0.0);
        return;
    }
    soloInPiece_ = true;
    double* s0 = solo_.data() + off;
    double* s1 = solo_.data() + maxFrames_ + off;
    const double* d0 = dry[0] + off;
    const double* d1 = dry[outCh >= 2 ? 1 : 0] + off;
    for (uint32_t i = 0; i < n; ++i) {
        mix[i] = soloMix_.next();
        const double lv = soloLevel_.next();
        double a = 0.0, c = 0.0;
        switch (soloPlacement_) {
            case 0:  // Stereo (and mono)
                a = dsp::svfTick(soloCoeffs_, soloState_[0], d0[i]);
                if (outCh >= 2) c = dsp::svfTick(soloCoeffs_, soloState_[1], d1[i]);
                break;
            case 1: a = dsp::svfTick(soloCoeffs_, soloState_[0], d0[i]); break;  // Left
            case 2: c = dsp::svfTick(soloCoeffs_, soloState_[1], d1[i]); break;  // Right
            case 3: a = c = dsp::svfTick(soloCoeffs_, soloState_[0], 0.5 * (d0[i] + d1[i])); break;  // Mid
            case 4:  // Side
                a = dsp::svfTick(soloCoeffs_, soloState_[1], 0.5 * (d0[i] - d1[i]));
                c = -a;
                break;
            default: break;
        }
        s0[i] = a * lv;
        s1[i] = c * lv;
    }
}

double Engine::bandValue(int band, BandField f) const noexcept {
    const int k = rampSlot(f);
    if (band < 0 || band >= params::kBandCount || k < 0) return 0.0;
    return field(band, k).ramp.current();
}

}  // namespace amq::engine
