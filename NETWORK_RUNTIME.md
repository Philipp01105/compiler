# Private network runtime and core API

`stdlib/core/net` is the typed low-level TCP/UDP and DNS foundation. It requires the root manifest's
`features = ["async"]`. Public `stdlib/net`, TLS, HTTP and socket convenience options remain future work.

## Selection and linking

Network intrinsics in emitted IR require `NETWORK`, which implies `PLATFORM_RUNTIME`. Merely importing this package,
enabling async or passing `--link=external` does not require networking. Whole-program emission retains user-unit
functions and necessary package lifecycle, vtable and destructor functions; unused standard-library functions are
excluded. Library-object emission retains its functions. Requirements describe emitted operations, including retained
function bodies, rather than predicting which branches execute.

`--link=auto` uses external linking for network programs. `--link=internal` rejects their platform profile. Object and
assembly emission use the same private ABI but start no external process. CMake builds and installs the combined
`dmm-runtime/elf/network-shim.a` or `dmm-runtime/coff/network-shim.a` beside the compiler. This archive includes the DMM
platform primitives and the remaining network C object; do not link both runtime bundles.
`--runtime-shim PATH` replaces the complete required archive. Regular C startup calls compiler-generated `main`.
Cross-linking requires an explicit matching GCC-compatible driver and shim. Only network programs add Windows
`ws2_32`; pure platform output uses the smaller `platform-shim.o`. Standalone startup and dependencies remain unchanged.

```sh
compiler program.dmm --linker-driver gcc -o program
compiler program.dmm --emit=obj -o program.o
# Linux regular C startup
gcc -no-pie program.o /path/to/dmm-runtime/elf/network-shim.a -pthread -o program
# Windows MinGW-w64 UCRT64, GCC or Clang
gcc program.obj C:/path/dmm-runtime/coff/network-shim.a -lws2_32 -o program.exe
# Assembly also uses ordinary startup; for an arbitrary suffix:
gcc -x assembler program.asm -x none /path/to/network-shim.a -pthread -no-pie -o program
```

## Typed operations and ownership

The source package defines `AddressFamily.IPv4|IPv6`, `Transport.TCP|UDP`, `IPv4`, `IPv6`, `Address`, `Endpoint`,
`Deadline`, `NetError`, `Socket` and `Addresses`. IPv4 numeric octets use network byte order: `2130706433` is
127.0.0.1. IPv6 uses two numeric 64-bit halves and an interface scope index. `ipv6(0,1,0,port)` is loopback.
IPv6 sockets always set `IPV6_V6ONLY`; families remain separate. Port zero lets the OS select a port.

| Operation | Contract |
| --- | --- |
| `socket(family,transport)` | `Result<Socket,NetError>`; move-only private handle |
| `bind(&mut socket,endpoint)`, `listen(&mut socket,backlog)` | Exclusive loan, synchronous Result |
| `localAddress(&socket)`, `peerAddress(&socket)` | Endpoint Result; requires bound/connected state respectively |
| `shutdown(&mut socket,Shutdown.Read|Write|Both)` | Exclusive loan, synchronous Result |
| `close(socket)` | Consumes ownership, synchronous `Result<void,NetError>` |
| `connect(&socket,endpoint,deadline)` | Future of Result<void>; both slots |
| `accept(&listener,deadline)` | Future of Result<Accepted>; read slot; owned socket and peer |
| `read(&socket,&mut bytes,deadline)` | Future of Result<ReadResult>; bytes and EOF |
| `write(&socket,&bytes,deadline)` | Future of Result<usize>; short transfers preserved |
| `recvFrom(&socket,&mut bytes,deadline)` | Future of Result<Datagram>; bytes, peer, truncation |
| `sendTo(&socket,&bytes,endpoint,deadline)` | Future of Result<usize>; one datagram |
| `resolve(hostname,port,family,transport,deadline)` | Future of Result<Addresses>; copied inputs |
| `addresses.length()`, `addresses.get(index)` | Count and Option<Endpoint>; list owns its allocation |

All Results use `stdlib.Result`. Socket and address-list destructors relinquish their remaining ownership once.
Explicit close reports errors and invalidates ownership even if the OS close fails; the destructor ignores errors and
never retries an explicitly consumed owner. Active future loans prohibit close, move, destruction and conflicting
buffer accesses. Checked references to slice descriptors trace their views to the underlying storage owner.

Shared socket loans guarantee memory/lifetime safety, **not operation-slot exclusivity**. Each socket has one read
and one write slot: full duplex is allowed; another operation occupying a claimed direction returns `Busy`. Connect
claims both; Accept claims the listener's read slot. Slot acquisition happens when a socket operation's lazy future
first polls. Invalid socket states return `InvalidState`. An admitted failed, cancelled or expired Connect closes and
invalidates its socket; a new connection attempt needs a new owner. A conflicting `Busy` request does not invalidate
the operation already owning its slots.

TCP preserves short transfers. EOF is a zero-byte result from a nonempty read; requesting zero bytes returns zero
without EOF. UDP preserves datagrams, including empty datagrams; truncated receives report copied bytes and the
truncation flag. Normalized errors include Busy, TimedOut, Closed, InvalidState, InvalidInput, Refused, Reset,
Unreachable, AddressInUse, OutOfMemory and System. `NetError.domain` and signed `code` preserve Runtime, Posix,
Winsock, Win32 or Resolver provenance.

`Deadline.noTimeout()` represents Never; `Deadline.at(monotonicNow()+...)` uses absolute nanoseconds.
`Deadline.after(durationNanoseconds)` computes its absolute timestamp once, with overflow saturation below Never.
Retries never extend a deadline. An expired deadline performs no new socket I/O or resolver job. Monotonic clocks
use CLOCK_MONOTONIC or QueryPerformanceCounter with checked conversion/addition.

```dmm
package main;
import "stdlib";
import "stdlib/core/net";
async func probe()->stdlib.Result<usize,net.NetError> {
    var socket:net.Socket=net.socket(net.AddressFamily.IPv4,net.Transport.UDP)?;
    net.bind(&mut socket,net.ipv4(2130706433,0))?;
    var endpoint=net.localAddress(&socket)?;
    var buffer:u8[4096]; var view:u8[]=buffer[:];
    var packet=net.recvFrom(&socket,&mut view,net.Deadline.after(1000000000)).await()?;
    net.close(socket)?;
    return stdlib.Result<usize,net.NetError>.Ok(packet.bytes);
}
```

The example's own socket and fixed buffer belong to its pinned future graph, so the complete future can be spawned.
The independently borrowed receive child cannot be spawned. See `tests/network/loopback.dmm` for a runnable
IPv4/IPv6 TCP/UDP and DNS example, and `tests/network/lifecycle.dmm` for package ownership and executor drain.

## Private ABI and completion

`src/runtime/network_shim.h` defines x86-64 private C ABI v1. POD layouts have static assertions: address is 40 bytes
(family, port, scope as host-order u64 followed by 16 network-order bytes); error is 24 bytes; request is 96 bytes.
Typed intrinsic descriptors validate argument/result shapes and NETWORK requirements in IR. DMM wrappers explicitly
encode requests and translate POD results to DMM enums/structs. No DMM enum layout or native sockaddr crosses this
boundary. Handles and C allocations stay shim-owned; DMM-generated frames use their own allocator.

Each stable operation allocation owns the **generated** runtime's `__dmm_async_io_create(NULL,NULL)` acknowledgement.
The private 80-byte Future<void> header polls that acknowledgement and requests cancellation through its operation;
the enclosing DMM Operation guard retains ownership until acknowledgement. This does not use `executor.c`.
Completion, timeout and cancellation publish once under the reactor lock. Notification and wakers run outside that
lock. A published result never changes. Cancellation cannot free a buffer while the OS may still access it, and
already transferred bytes are not rolled back.

Linux uses nonblocking CLOEXEC sockets, level-triggered epoll, eventfd commands and a monotonic deadline heap.
Readiness events retain no operation pointers or socket handles: they schedule a scan of current stable operations,
so stale readiness and descriptor reuse cannot address freed objects. EINTR retries; EAGAIN registers readiness;
Connect checks SO_ERROR and confirmed peer state; sends suppress SIGPIPE.

Windows supports MinGW-w64 **UCRT64** GCC/Clang. Overlapped sockets use IOCP, AcceptEx, ConnectEx, WSARecv/WSASend
and their datagram counterparts. Connect binds when needed and updates its context; Accept updates its listener
context. OVERLAPPED, buffers, flags, lengths and address outputs remain in stable operation storage until the packet
is consumed. Neither successful CancelIoEx nor ERROR_NOT_FOUND permits early reclamation, as required by the
[Microsoft cancellation contract](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).
System thread creation remains `_beginthreadex` through the platform shim.

## DNS and lifecycle

Two lazy resolver workers accept at most 64 waiting jobs. Full queues return Busy. Hostname and request inputs are
copied synchronously when `resolve` constructs its future; cancellation before its first poll is supported. Queued
cancellations remove their jobs. Running cancellations disconnect and acknowledge the caller while a private worker
reference retains copied inputs and frees late answers. Numeric loopback inputs and injectable worker/reactor gates
make the tests independent of external DNS. A blocking system resolver never blocks an executor worker; final
shutdown joins resolver workers and can wait for the resolver to return.

Normal generated startup follows this dependency order:

1. Runtime storage is available; packages initialize and DMM main runs.
2. The default executor closes task admission and drains. Networking stays RUNNING; existing tasks may begin more
   I/O/DNS steps. Explicit executor Drain follows the same network availability rule.
3. After complete task drain, networking becomes DRAINING, before package cleanup. New sockets, DNS and socket
   operations return Closed. Package destructors may still close their owned handles.
4. After package cleanup, resolver workers finish/join, confirmed reactor work ends, and the reactor stops/joins.
   Windows calls WSACleanup last. Networking becomes SHUTDOWN, followed by remaining runtime cleanup.
5. The saved DMM exit code returns through `__dmm_runtime_main` to trivial C main; void main returns zero.

Winsock 2.2 initializes once under the shared runtime lock. Fatal exit, traps and fatal runtime errors retain immediate
process termination, with no guaranteed drain or cleanup.

## Validation

`network_shim_unit` covers IPv4/IPv6 TCP/UDP, full duplex, Busy, short transfer, EOF, empty/truncated datagrams,
submitted cancellation with a delayed completion consumer, immutable published results, deadlines, close-error
injection, TLS, bounded DNS, detached late results and shutdown. It checks no live acknowledgement/socket owners,
Linux descriptor balance and constant Windows handle counts across repeated socket passes. Windows Winsock's
process-wide caches are not mistaken for shim-owned resources. Linux ASan/UBSan with leak detection checks C ownership.

`network_runtime_contract` uses actual emitted runtime acknowledgements and spawnable owned frames, checked-loan
negative tests, package lifecycle, both emission paths, O0/O1 and Intel/AT&T. Host execution passes on Linux GCC and
Windows UCRT64 GCC/Clang. The existing platform/linker failure and standalone dependency suites remain enabled.
