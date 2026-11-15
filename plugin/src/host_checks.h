#pragma once
// Host-contract checking policy for Am-Q's CLAP plugin (docs/plan/decisions.md D1, area 01).
//
// Shipping builds: clap-helpers' Ignore handler with checking off. Under Terminate, a failed
// thread check calls std::terminate() and kills the host. VST3 hosts may legally call through
// clap-wrapper on threads CLAP forbids: Blue Cat's Axiom calls getLatencySamples() off the
// main thread, and that crashed the framework spike. Under Ignore the checks still build a
// message and call host.log on the calling thread, which allocates on the audio thread.
//
// Development builds (-DAMQ_STRICT_HOST_CHECKS=ON): Terminate + Maximal, so contract
// violations by our test hosts and the validators fail loudly.

#include <clap/helpers/plugin.hh>

namespace amq::plugin {

#if defined(AMQ_STRICT_HOST_CHECKS) && AMQ_STRICT_HOST_CHECKS
inline constexpr bool kStrictHostChecks = true;
using PluginBase = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                         clap::helpers::CheckingLevel::Maximal>;
#else
inline constexpr bool kStrictHostChecks = false;
using PluginBase = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Ignore,
                                         clap::helpers::CheckingLevel::None>;
#endif

}  // namespace amq::plugin
