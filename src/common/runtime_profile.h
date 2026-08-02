#ifndef DMM_RUNTIME_PROFILE_H
#define DMM_RUNTIME_PROFILE_H

typedef enum { LINK_AUTO, LINK_INTERNAL, LINK_EXTERNAL } LinkMode;
typedef unsigned RuntimeRequirements;
enum { RUNTIME_REQUIRE_PLATFORM = 1u, RUNTIME_REQUIRE_NETWORK = 2u };
typedef enum { RUNTIME_STANDALONE, RUNTIME_PLATFORM } RuntimeProfile;

/* Requirements describe operations, never the chosen linker. NETWORK is reserved
   for future IR intrinsics; async alone has no platform requirement. */
static inline RuntimeRequirements runtime_requirements_normalize(RuntimeRequirements requirements) {
    return requirements & RUNTIME_REQUIRE_NETWORK
        ? requirements | RUNTIME_REQUIRE_PLATFORM : requirements;
}
static inline RuntimeProfile runtime_profile_for(RuntimeRequirements requirements) {
    return runtime_requirements_normalize(requirements) & RUNTIME_REQUIRE_PLATFORM
        ? RUNTIME_PLATFORM : RUNTIME_STANDALONE;
}
static inline int runtime_resolve_link(LinkMode requested, RuntimeProfile profile, LinkMode *resolved) {
    if (requested == LINK_INTERNAL && profile == RUNTIME_PLATFORM) return 0;
    *resolved = requested == LINK_AUTO
        ? (profile == RUNTIME_PLATFORM ? LINK_EXTERNAL : LINK_INTERNAL) : requested;
    return 1;
}
#endif
