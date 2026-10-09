#ifndef DMM_NETWORK_SHIM_H
#define DMM_NETWORK_SHIM_H
#include <stdint.h>
#include <stddef.h>

/* Private networking ABI v1; never exposes native sockaddr or DMM enum layout.
   All integers are host order. IPv6 scope is an interface index. Handles are
   shim-owned; close consumes even on error. Shared socket loans protect life,
   whereas the read/write slots below are a runtime protocol (Busy on conflict).
   Operations own an emitted-runtime I/O acknowledgement. Release is legal only
   after completion/cancellation acknowledgement; DNS workers own copied input.
   No callback is invoked while the network mutex is held. */
enum DmmNetKind {
    DMM_NET_OK, DMM_NET_BUSY, DMM_NET_TIMEOUT, DMM_NET_CLOSED,
    DMM_NET_STATE, DMM_NET_INPUT, DMM_NET_REFUSED, DMM_NET_RESET,
    DMM_NET_UNREACHABLE, DMM_NET_ADDRESS_IN_USE, DMM_NET_MEMORY, DMM_NET_SYSTEM
};

enum DmmNetDomain { DMM_NET_RUNTIME, DMM_NET_POSIX, DMM_NET_WINSOCK, DMM_NET_RESOLVER, DMM_NET_WIN32 };

enum DmmNetCommand {
    DMM_NET_CONNECT = 1, DMM_NET_ACCEPT, DMM_NET_READ,
    DMM_NET_WRITE, DMM_NET_RECV_FROM, DMM_NET_SEND_TO, DMM_NET_DNS
};

typedef struct {
    uint64_t family, port, scope;
    unsigned char bytes[16];
} DmmNetAddress;

typedef struct {
    uint64_t kind, domain;
    int64_t code;
} DmmNetError;

typedef struct DmmNetSocket DmmNetSocket;
typedef struct DmmNetOperation DmmNetOperation;
typedef struct DmmNetAddresses DmmNetAddresses;

/* Packed request is explicit private POD, not a DMM struct passed by value. */
typedef struct {
    uint64_t command;
    DmmNetSocket *socket;
    void *buffer;
    uint64_t length, deadline;
    DmmNetAddress address;
    const char *hostname;
    uint64_t transport;
} DmmNetRequest;

_Static_assert (
sizeof
(
void *
)
==
8
,
"network ABI requires x86-64 pointers"
);
_Static_assert (
sizeof
(DmmNetAddress)
==
40
&&
offsetof(DmmNetAddress, bytes)
==
24
,
"network address ABI"
);
_Static_assert (
sizeof
(DmmNetError)
==
24
,
"network error ABI"
);
_Static_assert (
sizeof
(DmmNetRequest)
==
96
&&
offsetof(DmmNetRequest, address)
==
40
&&
offsetof(DmmNetRequest, hostname)
==
80
,
"network request ABI"
);
#define DMM_NET_NEVER UINT64_MAX

uint64_t __dmm_net_now(void);

uint64_t __dmm_net_pointer(void *);

DmmNetSocket *__dmm_net_socket(uint64_t family, uint64_t transport, DmmNetError *error);

int __dmm_net_bind(DmmNetSocket *, const DmmNetAddress *, DmmNetError *);

int __dmm_net_listen(DmmNetSocket *, uint64_t backlog, DmmNetError *);

int __dmm_net_address(DmmNetSocket *, uint64_t peer, DmmNetAddress *, DmmNetError *);

int __dmm_net_address_packet(DmmNetSocket *, uint64_t *packet, DmmNetError *);

int __dmm_net_shutdown_socket(DmmNetSocket *, uint64_t direction, DmmNetError *);

int __dmm_net_close(DmmNetSocket *, DmmNetError *);

DmmNetOperation *__dmm_net_start(const DmmNetRequest *);

void __dmm_net_cancel(DmmNetOperation *);

int __dmm_net_poll(DmmNetOperation *, void *context);

void __dmm_net_release(DmmNetOperation *);

/* Result fields: 0 kind,1 domain,2 code,3 count,4 EOF,5 truncated,6 socket,
   7 address-list. Address extraction uses a separate POD output buffer. */
uint64_t __dmm_net_result(DmmNetOperation *, uint64_t field);

void __dmm_net_result_address(DmmNetOperation *, DmmNetAddress *);

uint64_t __dmm_net_addresses_count(DmmNetAddresses *);

int __dmm_net_addresses_get(DmmNetAddresses *, uint64_t, DmmNetAddress *);

void __dmm_net_addresses_release(DmmNetAddresses *);

void *__dmm_net_wait(uint64_t operation);

void __dmm_net_begin_draining(void);

void __dmm_net_finish(void);
#endif
