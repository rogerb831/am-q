// Minimal VST3 test host for Am-Q.vst3, the clap-wrapper build of the CLAP plugin (area 01,
// docs/plan/decisions.md D1). It mirrors the CLAP host test (plugin_host_test.cpp) on the VST3
// side, where hosts reach the plugin through clap-wrapper:
//
//  1. Module, factory, one audio effect class, component and controller.
//  2. Buses: stereo main input, stereo side-chain aux input, stereo main output. Sample sizes:
//     32-bit only (pinned: clap-wrapper v0.17.0 refuses kSample64, see below). Latency 0.
//  3. Stereo pass-through is bit-exact with an odd block size; a loud side chain on the active
//     aux bus leaves the main output bit-exact (dynamics off).
//  4. Parameters: 574 exposed, ids equal the frozen CLAP host ids, one bypass parameter,
//     readable choices, text to value.
//  5. Sample-accurate automation: an Output Level point at offset k first changes the output
//     at sample k (offsets 0, 1, 37, 255, 511 in a 512-sample block).
//  6. State: a Band 1 Gain change made by host automation survives getState / setState into a
//     new instance.
//  7. Off-main-thread calls a VST3 host may make (latency, buses, parameters, value text,
//     state, editor size) return correct values and leave the state intact (area 01 task:
//     "Audit every VST3-reachable callback for any-thread safety").
//
//  8. Processing mode: automating Natural Phase reaches the host as restartComponent with
//     kLatencyChanged; after the host restarts the component the latency is 320 and a flat
//     impulse comes out 320 samples late (area 02 R9, R12, R13).
//
// Usage: amq_vst3_host_test <path to Am-Q.vst3>. Windows only for now.
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int gFailures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += !ok;
}

constexpr ParamID kBand01Freq = 1279620887u;  // tests/params/host_ids.golden.txt
constexpr int32 kParamCount = 574;

class Handler : public IComponentHandler {
public:
    int restarts = 0, latencyRestarts = 0;
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        ++restarts;
        if (flags & kLatencyChanged) ++latencyRestarts;
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override {
        if (FUnknownPrivate::iidEqual(queried, IComponentHandler::iid) || FUnknownPrivate::iidEqual(queried, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

// clap-wrapper services the plugin's main-thread requests (params.rescan after a state load,
// request_callback) from a window timer, so give it a few message-loop turns.
void pump(int ms) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
    while (GetTickCount64() < end) {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        Sleep(1);
    }
}

std::vector<float> noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto& x : v) x = u(rng);
    return v;
}

struct Plugin {
    IPtr<PlugProvider> provider;
    IComponent* comp = nullptr;
    IEditController* ctrl = nullptr;
    FUnknownPtr<IAudioProcessor> proc;
    Handler handler;
    HostProcessData data;
    int32 block = 512;
    bool active = false;

    bool open(VST3::Hosting::PluginFactory& factory) {
        for (auto& ci : factory.classInfos()) {
            if (ci.category() != kVstAudioEffectClass) continue;
            provider = owned(new PlugProvider(factory, ci, true));
            if (!provider->initialize()) return false;
            comp = provider->getComponentPtr();
            ctrl = provider->getControllerPtr();
            if (!comp || !ctrl) return false;
            proc = FUnknownPtr<IAudioProcessor>(comp);
            ctrl->setComponentHandler(&handler);
            return proc != nullptr;
        }
        return false;
    }
    void start(int32 blockSize, bool sidechain = false) {
        block = blockSize;
        ProcessSetup setup{kRealtime, kSample32, block, 48000.0};
        proc->setupProcessing(setup);
        comp->activateBus(kAudio, kInput, 0, true);
        comp->activateBus(kAudio, kInput, 1, sidechain);
        comp->activateBus(kAudio, kOutput, 0, true);
        comp->setActive(true);
        proc->setProcessing(true);
        data.unprepare();  // prepare() does not reallocate for a larger block size on its own
        data.prepare(*comp, block, kSample32);
        data.numSamples = block;
        active = true;
    }
    void stop() {
        if (!active) return;
        proc->setProcessing(false);
        comp->setActive(false);
        active = false;
    }
    ParamID find(const char* name) const {  // by CLAP name (VST3 shortTitle)
        for (int32 i = 0; i < ctrl->getParameterCount(); ++i) {
            ParameterInfo info{};
            ctrl->getParameterInfo(i, info);
            if (Steinberg::Vst::StringConvert::convert(info.shortTitle) == name) return info.id;
        }
        return kNoParamId;
    }
    // clap-wrapper does not convert between plain and normalized values (normalizedParamToPlain
    // returns its input), so the test sets and reads values through value text, as hosts do.
    ParamValue norm(ParamID id, const std::u16string& text) const {
        String128 s{};
        std::copy(text.begin(), text.end(), s);
        ParamValue v = -1;
        return ctrl->getParamValueByString(id, s, v) == kResultOk ? v : -1;
    }
    std::string text(ParamID id, ParamValue normalized) const {
        String128 s{};
        if (ctrl->getParamStringByValue(id, normalized, s) != kResultOk) return "<error>";
        return Steinberg::Vst::StringConvert::convert(s);
    }
    // One block: L and R inputs on the main bus, an optional side chain, optional changes.
    // Returns the left and right main outputs.
    std::vector<float> run(const std::vector<float>& l, const std::vector<float>& r, ParameterChanges* changes,
                           const std::vector<float>* sc = nullptr) {
        std::copy(l.begin(), l.end(), data.inputs[0].channelBuffers32[0]);
        std::copy(r.begin(), r.end(), data.inputs[0].channelBuffers32[1]);
        if (data.numInputs > 1)
            for (int32 c = 0; c < data.inputs[1].numChannels; ++c)
                for (int32 i = 0; i < block; ++i) data.inputs[1].channelBuffers32[c][i] = sc ? (*sc)[static_cast<size_t>(i)] : 0.0f;
        for (int32 c = 0; c < data.outputs[0].numChannels; ++c)
            std::fill(data.outputs[0].channelBuffers32[c], data.outputs[0].channelBuffers32[c] + block, 9.0f);
        ParameterChanges empty;
        ParameterChanges out;
        data.inputParameterChanges = changes ? changes : &empty;
        data.outputParameterChanges = &out;
        proc->process(data);
        std::vector<float> o(data.outputs[0].channelBuffers32[0], data.outputs[0].channelBuffers32[0] + block);
        o.insert(o.end(), data.outputs[0].channelBuffers32[1], data.outputs[0].channelBuffers32[1] + block);
        return o;
    }
};

// Streams are reference counted and the plugin may addRef/release them, so never on the stack.
IPtr<MemoryStream> stream() { return owned(new MemoryStream); }

void addPoint(ParameterChanges& ch, ParamID id, int32 offset, ParamValue normalized) {
    int32 qi = 0, pi = 0;
    if (auto* q = ch.addParameterData(id, qi)) q->addPoint(offset, normalized, pi);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: amq_vst3_host_test <Am-Q.vst3>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep output up to a crash
    (void)CoInitialize(nullptr);
    std::string err;
    auto module = VST3::Hosting::Module::create(argv[1], err);
    if (!module) {
        std::printf("FAIL: cannot load module %s: %s\n", argv[1], err.c_str());
        return 1;
    }
    HostApplication hostApp;
    PluginContextFactory::instance().setPluginContext(&hostApp);
    auto factory = module->getFactory();

    // 1. Factory, class, component and controller.
    int effects = 0;
    for (auto& ci : factory.classInfos()) effects += ci.category() == kVstAudioEffectClass;
    check(effects == 1, "factory has one audio effect class");
    Plugin p;
    if (!p.open(factory)) {
        std::printf("FAIL: cannot create the component and controller\n");
        return 1;
    }
    check(true, "component and controller created and initialised");

    // 2. Buses, sample sizes, latency.
    BusInfo bi{};
    check(p.comp->getBusCount(kAudio, kInput) == 2 && p.comp->getBusInfo(kAudio, kInput, 0, bi) == kResultOk &&
              bi.channelCount == 2 && bi.busType == kMain,
          "two input buses: the first is the stereo main input");
    check(p.comp->getBusInfo(kAudio, kInput, 1, bi) == kResultOk && bi.channelCount == 2 && bi.busType == kAux,
          "the second input bus is a stereo aux (side chain, area 01 R2)");
    check(p.comp->getBusCount(kAudio, kOutput) == 1 && p.comp->getBusInfo(kAudio, kOutput, 0, bi) == kResultOk &&
              bi.channelCount == 2 && bi.busType == kMain,
          "one stereo main output bus");
    check(p.comp->getBusCount(kEvent, kInput) == 0 && p.comp->getBusCount(kEvent, kOutput) == 0, "no event buses");
    // clap-wrapper v0.17.0 hard-codes VST3 to 32-bit (ClapAsVst3::canProcessSampleSize), although
    // the CLAP plugin takes 64-bit buffers. Pinned so that a wrapper upgrade that changes it shows.
    check(p.proc->canProcessSampleSize(kSample32) == kResultOk && p.proc->canProcessSampleSize(kSample64) != kResultOk,
          "32-bit processing only (clap-wrapper v0.17.0 limitation; the CLAP build takes 64-bit)");
    p.start(333);
    check(p.proc->getLatencySamples() == 0, "latency 0 after activation");

    // 3. Pass-through with an odd block size; side chain does not reach the main output.
    const auto inL = noise(333, 7), inR = noise(333, 8);
    {
        const auto o = p.run(inL, inR, nullptr);
        check(std::equal(inL.begin(), inL.end(), o.begin()) && std::equal(inR.begin(), inR.end(), o.begin() + 333),
              "stereo pass-through is bit-exact (333-sample block)");
    }
    p.stop();
    {
        Plugin q;
        q.open(factory);
        q.start(333, true);
        std::vector<float> sc(333);
        for (size_t i = 0; i < sc.size(); ++i) sc[i] = (i & 1) ? 0.99f : -0.99f;
        const auto o = q.run(inL, inR, nullptr, &sc);
        check(q.data.numInputs == 2 &&
                  std::equal(inL.begin(), inL.end(), o.begin()) && std::equal(inR.begin(), inR.end(), o.begin() + 333),
              "loud side chain on the active aux bus: main output unchanged");
        q.stop();
    }

    // 4. Parameters.
    const int32 count = p.ctrl->getParameterCount();
    check(count == kParamCount, "574 parameters exposed (24 bands x 23 fields + 22 globals)");
    std::vector<ParamID> ids;
    int bypass = 0;
    bool idsOk = true, hasFreq = false;
    for (int32 i = 0; i < count; ++i) {
        ParameterInfo info{};
        if (p.ctrl->getParameterInfo(i, info) != kResultOk) { idsOk = false; break; }
        if (std::find(ids.begin(), ids.end(), info.id) != ids.end() || info.id >= 0x80000000u) {
            std::printf("  parameter %d: duplicate or out-of-range id %u\n", i, info.id);
            idsOk = false;
        }
        ids.push_back(info.id);
        bypass += (info.flags & ParameterInfo::kIsBypass) != 0;
        if (info.id == kBand01Freq) {
            hasFreq = true;
            const std::string def = p.text(info.id, info.defaultNormalizedValue);
            if (def != "1.00 kHz") {
                std::printf("  band01.freq default reads \"%s\", expected \"1.00 kHz\"\n", def.c_str());
                idsOk = false;
            }
        }
    }
    if (!hasFreq || bypass != 1) std::printf("  band01.freq found: %d, bypass parameters: %d\n", hasFreq, bypass);
    check(idsOk && hasFreq && bypass == 1,
          "ids unique and below 2^31, band01.freq keeps its frozen CLAP id (default 1 kHz), one bypass parameter");
    const ParamID shapeId = p.find("Band 1 Shape"), gainId = p.find("Band 1 Gain"), levelId = p.find("Output Level");
    check(shapeId != kNoParamId && gainId != kNoParamId && levelId != kNoParamId, "Band 1 Shape, Band 1 Gain and Output Level found");
    {
        ParameterInfo si{};
        for (int32 i = 0; i < count; ++i) {
            p.ctrl->getParameterInfo(i, si);
            if (si.id == shapeId) break;
        }
        // VST3 normalizes a stepped value as step / stepCount.
        const bool stepped = si.stepCount >= 9;
        check(stepped && p.text(shapeId, 0.0) == "Bell" && p.text(shapeId, 9.0 / si.stepCount) == "All Pass",
              "Shape is a stepped choice; value text Bell .. All Pass");
    }
    {
        const ParamValue nv = p.norm(gainId, u"+6 dB");
        const std::string back = p.text(gainId, nv);
        if (back != "+6.0 dB") std::printf("  \"+6 dB\" parsed to %.12g, which reads \"%s\"\n", nv, back.c_str());
        check(back == "+6.0 dB", "text to value: Band 1 Gain \"+6 dB\" reads back \"+6.0 dB\"");
    }

    // 5. Sample-accurate automation through IParameterChanges.
    {
        bool exact = levelId != kNoParamId;
        const auto xl = noise(512, 11), xr = noise(512, 12);
        for (int32 k : {0, 1, 37, 255, 511}) {
            std::vector<float> outs[2];
            for (int withEvent = 0; withEvent < 2; ++withEvent) {
                Plugin q;
                q.open(factory);
                q.start(512);
                ParameterChanges ch;
                if (withEvent) addPoint(ch, levelId, k, 0.5);  // about -3.6 dB
                outs[withEvent] = q.run(xl, xr, &ch);
                q.stop();
            }
            int first = -1;
            for (size_t i = 0; i < 512; ++i)
                if (outs[0][i] != outs[1][i]) { first = static_cast<int>(i); break; }
            if (first != k) {
                std::printf("  offset %d: first changed sample %d\n", k, first);
                exact = false;
            }
        }
        check(exact, "Output Level automation lands on the exact sample (offsets 0, 1, 37, 255, 511)");
    }

    // 6. State: host automation sets Band 1 Gain to 7.5 dB; getState / setState carries it.
    const ParamValue gain75 = p.norm(gainId, u"+7.5 dB");
    auto gainIs75 = [&](const Plugin& x) {
        const ParamValue n = x.ctrl->getParamNormalized(gainId);
        const std::string t = x.text(gainId, n);
        const bool ok = std::abs(n - gain75) <= 1e-9 && t == "+7.5 dB";
        if (!ok) std::printf("  Band 1 Gain is %.12g (\"%s\"), expected %.12g (\"+7.50 dB\")\n", n, t.c_str(), gain75);
        return ok;
    };
    {
        p.start(512);
        ParameterChanges ch;
        addPoint(ch, gainId, 10, gain75);
        p.run(noise(512, 3), noise(512, 4), &ch);
        p.stop();
        pump(100);
        auto ms = stream();
        const bool saved = p.comp->getState(ms) == kResultOk && ms->getSize() > 0;
        Plugin q;
        q.open(factory);
        ms->seek(0, IBStream::kIBSeekSet, nullptr);
        const bool loaded = q.comp->setState(ms) == kResultOk;
        ms->seek(0, IBStream::kIBSeekSet, nullptr);
        q.ctrl->setComponentState(ms);
        pump(100);
        check(saved && loaded && gainIs75(q), "state round trip into a new instance keeps Band 1 Gain at +7.5 dB");
        auto junk = stream();
        const char bytes[] = "not an Am-Q state chunk";
        int32 written = 0;
        junk->write(const_cast<char*>(bytes), sizeof(bytes), &written);
        junk->seek(0, IBStream::kIBSeekSet, nullptr);
        check(q.comp->setState(junk) != kResultOk, "setState rejects foreign data");
    }

    // 7. Off-main-thread calls. The editor view is created and released on the main thread; only
    //    getSize runs on the other thread.
    {
        p.start(512);
        IPtr<IPlugView> view = owned(p.ctrl->createView(ViewType::kEditor));
        uint32 latOff = 99;
        int32 busesOff = 0, countOff = 0;
        bool paramOff = false, textOff = false, saveOff = false, loadOff = false, sizeOff = false;
        ViewRect rect{};
        std::thread t([&] {
            latOff = p.proc->getLatencySamples();
            BusInfo b{};
            busesOff = p.comp->getBusCount(kAudio, kInput) + p.comp->getBusCount(kAudio, kOutput);
            busesOff *= p.comp->getBusInfo(kAudio, kInput, 1, b) == kResultOk && b.busType == kAux;
            countOff = p.ctrl->getParameterCount();
            ParameterInfo info{};
            paramOff = p.ctrl->getParameterInfo(3, info) == kResultOk && p.ctrl->getParamNormalized(info.id) >= 0.0;
            String128 s{};
            ParamValue back = -1;
            textOff = p.ctrl->getParamStringByValue(gainId, gain75, s) == kResultOk &&
                      p.ctrl->getParamValueByString(gainId, s, back) == kResultOk && std::abs(back - gain75) < 1e-9;
            auto ms = stream();
            saveOff = p.comp->getState(ms) == kResultOk && ms->getSize() > 0;
            ms->seek(0, IBStream::kIBSeekSet, nullptr);
            loadOff = p.comp->setState(ms) == kResultOk;
            sizeOff = view && view->getSize(&rect) == kResultOk;
        });
        t.join();
        view = nullptr;
        pump(100);
        check(latOff == 0 && busesOff == 3 && countOff == kParamCount && paramOff && textOff,
              "off-main-thread latency, bus, parameter and value-text calls");
        check(saveOff && loadOff, "off-main-thread getState and setState");
        check(sizeOff && rect.getWidth() > 0 && rect.getHeight() > 0, "off-main-thread editor getSize");
        check(gainIs75(p), "off-thread setState kept Band 1 Gain at +7.50 dB");
        p.stop();
    }

    // 8. Processing mode and the latency notification.
    {
        Plugin q;
        q.open(factory);
        const ParamID modeId = q.find("Processing Mode");
        const ParamValue np = q.norm(modeId, u"Natural Phase");
        q.start(512);
        const uint32 before = q.proc->getLatencySamples();
        ParameterChanges ch;
        addPoint(ch, modeId, 0, np);
        q.run(noise(512, 5), noise(512, 6), &ch);
        q.run(std::vector<float>(512, 0.0f), std::vector<float>(512, 0.0f), nullptr);
        pump(300);  // clap-wrapper sends restartComponent from its idle timer
        const int latencyRestarts = q.handler.latencyRestarts;
        q.stop();   // the host's answer: restart the component
        q.start(512);
        const uint32 after = q.proc->getLatencySamples();
        q.run(std::vector<float>(512, 0.0f), std::vector<float>(512, 0.0f), nullptr);
        std::vector<float> imp(512, 0.0f);
        imp[0] = 1.0f;
        const auto y = q.run(imp, std::vector<float>(512, 0.0f), nullptr);
        int peak = 0;
        for (int i = 1; i < 512; ++i)
            if (std::abs(y[static_cast<size_t>(i)]) > std::abs(y[static_cast<size_t>(peak)])) peak = i;
        if (before != 0 || latencyRestarts < 1 || after != 320 || peak != 320)
            std::printf("  latency before %u, kLatencyChanged restarts %d, after %u, impulse peak at %d\n", before,
                        latencyRestarts, after, peak);
        check(before == 0 && latencyRestarts >= 1, "Natural Phase automation reaches the host as restartComponent(kLatencyChanged)");
        check(after == 320 && peak == 320 && std::abs(y[320] - 1.0f) <= 1e-6f,
              "after the restart: latency 320, flat impulse exactly 320 samples late");

        // The host's bypass parameter (kIsBypass): with Output Level turned down, bypass gives
        // the input delayed by the latency, bit for bit (area 01 R6, area 09 R7).
        ParamID bypassId = kNoParamId;
        for (int32 i = 0; i < q.ctrl->getParameterCount(); ++i) {
            ParameterInfo info{};
            q.ctrl->getParameterInfo(i, info);
            if (info.flags & ParameterInfo::kIsBypass) bypassId = info.id;
        }
        const std::vector<float> silence(512, 0.0f);
        ParameterChanges quiet;
        addPoint(quiet, q.find("Output Level"), 0, 0.25);
        q.run(silence, silence, &quiet);
        q.run(silence, silence, nullptr);
        const auto wet = q.run(imp, silence, nullptr);
        ParameterChanges byp;
        addPoint(byp, bypassId, 0, 1.0);
        for (int b = 0; b < 3; ++b) q.run(silence, silence, b == 0 ? &byp : nullptr);
        const auto dry = q.run(imp, silence, nullptr);
        bool exact = true;
        for (int i = 0; i < 512; ++i) exact &= dry[static_cast<size_t>(i)] == (i == 320 ? 1.0f : 0.0f);
        check(bypassId != kNoParamId && std::abs(wet[320]) < 0.5f && exact,
              "host bypass (kIsBypass): input delayed by 320 samples, bit for bit");
        q.stop();
    }

    // Release everything that points into the module before unloading it.
    p.proc = FUnknownPtr<IAudioProcessor>();
    p.provider = nullptr;
    factory = VST3::Hosting::PluginFactory(nullptr);
    PluginContextFactory::instance().setPluginContext(nullptr);
    module.reset();
    CoUninitialize();
    std::printf("%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");
    return gFailures ? 1 : 0;
}
