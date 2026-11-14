// Exported clap_entry, compiled into each format by clap-wrapper's make_clapfirst_plugins.
#include <clap/clap.h>

extern "C" {
bool amq_spike_entry_init(const char* path);
void amq_spike_entry_deinit();
const void* amq_spike_entry_get_factory(const char* id);

CLAP_EXPORT const clap_plugin_entry clap_entry = {CLAP_VERSION, amq_spike_entry_init, amq_spike_entry_deinit,
                                                  amq_spike_entry_get_factory};
}
