// Minimal VST3 test host for the Am-Q spike (evidence for pass criteria 1 and 2, VST3 side).
//  1. Sample-accurate automation: a parameter point at offset k inside a block must change
//     the output starting exactly at sample k (compared with a run without the point).
//  2. Latency change: toggling the dummy-latency parameter must reach the host as
//     restartComponent(kLatencyChanged), and getLatencySamples() must report 64 after the
//     host restarts the component; an impulse must come out 64 samples late.
// Usage: vst3_host_test <path to AmQSpike.vst3>
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

class Handler : public IComponentHandler {
public:
    int restarts = 0, latencyFlags = 0, edits = 0;
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { ++edits; return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        ++restarts;
        if (flags & kLatencyChanged) ++latencyFlags;
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

void pump(int ms) {
    const DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        Sleep(1);
    }
}

struct Plugin {
    IPtr<PlugProvider> provider;
    IComponent* comp = nullptr;
    IEditController* ctrl = nullptr;
    FUnknownPtr<IAudioProcessor> proc;
    Handler handler;
    HostProcessData data;
    int32 block = 256;

    bool open(VST3::Hosting::PluginFactory& factory) {
        for (auto& ci : factory.classInfos()) {
            if (ci.category() != kVstAudioEffectClass) continue;
            provider = owned(new PlugProvider(factory, ci, true));
            if (!provider->initialize()) return false;
            comp = provider->getComponentPtr();
            ctrl = provider->getControllerPtr();
            proc = FUnknownPtr<IAudioProcessor>(comp);
            ctrl->setComponentHandler(&handler);
            return proc != nullptr;
        }
        return false;
    }
    void start() {
        ProcessSetup setup{kRealtime, kSample32, block, 48000.0};
        proc->setupProcessing(setup);
        comp->activateBus(kAudio, kInput, 0, true);
        comp->activateBus(kAudio, kOutput, 0, true);
        comp->setActive(true);
        proc->setProcessing(true);
        data.prepare(*comp, block, kSample32);
        data.numSamples = block;
    }
    void stop() {
        proc->setProcessing(false);
        comp->setActive(false);
    }
    ParamID findParam(const char* title) {
        for (int32 i = 0; i < ctrl->getParameterCount(); ++i) {
            ParameterInfo info{};
            ctrl->getParameterInfo(i, info);
            if (VST3::StringConvert::convert(info.title) == title) return info.id;
        }
        return 0xffffffff;
    }
    std::vector<float> run(const std::vector<float>& in, ParameterChanges* changes) {
        for (int32 c = 0; c < data.inputs[0].numChannels; ++c)
            std::copy(in.begin(), in.end(), data.inputs[0].channelBuffers32[c]);
        ParameterChanges empty;
        data.inputParameterChanges = changes ? changes : &empty;
        ParameterChanges out;
        data.outputParameterChanges = &out;
        proc->process(data);
        return std::vector<float>(data.outputs[0].channelBuffers32[0], data.outputs[0].channelBuffers32[0] + block);
    }
};

std::vector<float> noise(size_t n, uint32_t seed) {
    std::vector<float> v(n);
    for (auto& x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<float>((seed >> 8) * (1.0 / 16777216.0) - 0.5);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: vst3_host_test <plugin.vst3>\n");
        return 2;
    }
    CoInitialize(nullptr);
    std::string err;
    auto module = VST3::Hosting::Module::create(argv[1], err);
    if (!module) {
        std::printf("FAIL: cannot load module: %s\n", err.c_str());
        return 1;
    }
    HostApplication hostApp;
    PluginContextFactory::instance().setPluginContext(&hostApp);
    auto factory = module->getFactory();
    int failures = 0;

    struct Step { const char* name; const char* title; double plain; uint32_t offset; };
    const Step steps[] = {{"Output Gain -6 dB", "Output Gain", -6.0, 37}, {"Band Gain +12 dB", "Gain", 12.0, 101},
                          {"Output Gain at offset 0", "Output Gain", 3.0, 0}, {"Output Gain at last sample", "Output Gain", -3.0, 255}};
    for (const auto& s : steps) {
        std::vector<float> outs[2];
        for (int withEvent = 0; withEvent < 2; ++withEvent) {
            Plugin p;
            if (!p.open(factory)) { std::printf("FAIL: cannot open plugin\n"); return 1; }
            p.start();
            const ParamID id = p.findParam(s.title);
            for (int b = 0; b < 3; ++b) {
                ParameterChanges ch;
                if (withEvent && b == 1) {
                    int32 qi = 0, pi = 0;
                    auto* q = ch.addParameterData(id, qi);
                    q->addPoint(static_cast<int32>(s.offset), p.ctrl->plainParamToNormalized(id, s.plain), pi);
                }
                const auto o = p.run(noise(p.block, 1234u + b), &ch);
                outs[withEvent].insert(outs[withEvent].end(), o.begin(), o.end());
            }
            p.stop();
        }
        int first = -1;
        for (size_t i = 0; i < outs[0].size(); ++i)
            if (outs[0][i] != outs[1][i]) { first = static_cast<int>(i); break; }
        const int expected = 256 + static_cast<int>(s.offset);
        const bool ok = first == expected;
        failures += !ok;
        std::printf("[%s] VST3 automation step '%s' at block 1 offset %u: first changed sample %d, expected %d\n",
                    ok ? "PASS" : "FAIL", s.name, s.offset, first, expected);
    }

    {
        Plugin p;
        p.open(factory);
        p.start();
        const uint32 before = p.proc->getLatencySamples();
        const ParamID lat = p.findParam("Dummy Latency");
        ParameterChanges ch;
        int32 qi = 0, pi = 0;
        ch.addParameterData(lat, qi)->addPoint(10, 1.0, pi);
        p.run(std::vector<float>(p.block, 0.0f), &ch);
        pump(300);  // the wrapper services request_callback from a 20 ms window timer
        const int restartsSeen = p.handler.restarts, latencyFlagsSeen = p.handler.latencyFlags;
        // Host reaction to restartComponent(kLatencyChanged): restart the component.
        p.stop();
        p.start();
        pump(100);
        const uint32 after = p.proc->getLatencySamples();
        std::vector<float> imp(p.block, 0.0f);
        imp[0] = 1.0f;
        const auto out = p.run(imp, nullptr);
        int peak = 0;
        for (int i = 1; i < static_cast<int>(out.size()); ++i)
            if (std::abs(out[i]) > std::abs(out[peak])) peak = i;
        const bool ok = before == 0 && latencyFlagsSeen >= 1 && after == 64 && peak == 64;
        failures += !ok;
        std::printf("[%s] VST3 latency: before %u, restartComponent calls %d (with kLatencyChanged: %d) before restart, "
                    "total kLatencyChanged %d, after %u, impulse peak at %d\n",
                    ok ? "PASS" : "FAIL", before, restartsSeen, latencyFlagsSeen, p.handler.latencyFlags, after, peak);
        p.stop();
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    CoUninitialize();
    return failures ? 1 : 0;
}
