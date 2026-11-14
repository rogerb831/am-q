// Factory and entry functions for the Am-Q spike, in the static implementation library.
// src/clap_entry.cpp turns them into the exported clap_entry for each format
// (clap-wrapper "clap-first" pattern).
#include "spike_plugin.h"

#include <cstring>

namespace {

uint32_t factoryCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* factoryDescriptor(const clap_plugin_factory*, uint32_t index) {
    return index == 0 ? &amq::spike::SpikePlugin::descriptor : nullptr;
}

const clap_plugin* factoryCreate(const clap_plugin_factory*, const clap_host* host, const char* id) {
    if (std::strcmp(id, amq::spike::SpikePlugin::descriptor.id) != 0) return nullptr;
    auto* p = new amq::spike::SpikePlugin(host);  // owned by the clap_plugin (destroy deletes it)
    return p->clapPlugin();
}

const clap_plugin_factory kFactory = {factoryCount, factoryDescriptor, factoryCreate};

}  // namespace

extern "C" {
bool amq_spike_entry_init(const char*) { return true; }
void amq_spike_entry_deinit() {}
const void* amq_spike_entry_get_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}
}
