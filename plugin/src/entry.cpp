// Factory and entry functions, in the static implementation library. src/clap_entry.cpp turns
// them into the exported clap_entry of each format (clap-wrapper clap-first pattern).
#include "amq_plugin.h"

#include <cstring>

namespace {

uint32_t factoryCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* factoryDescriptor(const clap_plugin_factory*, uint32_t index) {
    return index == 0 ? &amq::plugin::AmqPlugin::descriptor : nullptr;
}

const clap_plugin* factoryCreate(const clap_plugin_factory*, const clap_host* host, const char* id) {
    if (!id || std::strcmp(id, amq::plugin::AmqPlugin::descriptor.id) != 0) return nullptr;
    auto* p = new amq::plugin::AmqPlugin(host);  // owned by the clap_plugin; destroy deletes it
    return p->clapPlugin();
}

const clap_plugin_factory kFactory = {factoryCount, factoryDescriptor, factoryCreate};

}  // namespace

extern "C" {
bool amq_entry_init(const char*) { return true; }
void amq_entry_deinit() {}
const void* amq_entry_get_factory(const char* id) {
    return id && std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}
}
