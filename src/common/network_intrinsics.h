#ifndef DMM_NETWORK_INTRINSICS_H
#define DMM_NETWORK_INTRINSICS_H
/* Private scalar/POD transport descriptors. Public typed operations and loans
   live in core/net; this table neither exposes C signatures nor a public FFI. */
#define DMM_NETWORK_INTRINSICS(X) \
 X("__dmm_net_pointer", "__dmm_net_pointer", 1, CORE_SIZE, CORE_BYTES, CORE_VOID, CORE_VOID) \
 X("__dmm_net_now", "__dmm_net_now", 0, CORE_SIZE, CORE_VOID, CORE_VOID, CORE_VOID) \
 X("__dmm_net_socket", "__dmm_net_socket", 3, CORE_SIZE, CORE_SIZE, CORE_SIZE, CORE_BYTES) \
 X("__dmm_net_bind", "__dmm_net_bind", 3, CORE_INT, CORE_SIZE, CORE_BYTES, CORE_BYTES) \
 X("__dmm_net_listen", "__dmm_net_listen", 3, CORE_INT, CORE_SIZE, CORE_SIZE, CORE_BYTES) \
 X("__dmm_net_close", "__dmm_net_close", 2, CORE_INT, CORE_SIZE, CORE_BYTES, CORE_VOID) \
 X("__dmm_net_address_packet", "__dmm_net_address_packet", 3, CORE_INT, CORE_SIZE, CORE_BYTES, CORE_BYTES) \
 X("__dmm_net_shutdown_socket", "__dmm_net_shutdown_socket", 3, CORE_INT, CORE_SIZE, CORE_SIZE, CORE_BYTES) \
 X("__dmm_net_start", "__dmm_net_start", 1, CORE_SIZE, CORE_BYTES, CORE_VOID, CORE_VOID) \
 X("__dmm_net_release", "__dmm_net_release", 1, CORE_VOID, CORE_SIZE, CORE_VOID, CORE_VOID) \
 X("__dmm_net_result", "__dmm_net_result", 2, CORE_SIZE, CORE_SIZE, CORE_SIZE, CORE_VOID) \
 X("__dmm_net_result_address", "__dmm_net_result_address", 2, CORE_VOID, CORE_SIZE, CORE_BYTES, CORE_VOID) \
 X("__dmm_net_addresses_count", "__dmm_net_addresses_count", 1, CORE_SIZE, CORE_SIZE, CORE_VOID, CORE_VOID) \
 X("__dmm_net_addresses_get", "__dmm_net_addresses_get", 3, CORE_INT, CORE_SIZE, CORE_SIZE, CORE_BYTES) \
 X("__dmm_net_addresses_release", "__dmm_net_addresses_release", 1, CORE_VOID, CORE_SIZE, CORE_VOID, CORE_VOID)
#endif
