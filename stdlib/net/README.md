# Portable networking

Import "stdlib/net" with the root async feature. TcpStream, TcpListener and UdpSocket own OS resources through the existing reactor. Endpoint/Address, AddressFamily, Transport, Shutdown and Error are portable types.

## Connections and streams

dial(hostname,port) or dial(hostname,port,deadline) resolves both address families and attempts returned endpoints with one shared deadline. connect(endpoint,deadline) connects an explicit numeric endpoint. listen(endpoint) uses backlog 128; listen(endpoint,backlog) selects it explicitly. Listener.accept() or accept(deadline) yields Accepted with an owned stream and peer endpoint. localAddress/peerAddress return endpoint Results.

Stream read(&mut destination) and write(&source) report byte counts; deadline overloads bound the operation. Zero read on a nonempty destination is EOF, while empty reads also return zero. Short writes are normal. writeAll(&source[,deadline]) loops until complete, rejects zero progress with WriteZero and accumulates Error.transferred on failure. shutdown(direction) and shutdownWrite() report Results.

## Datagrams and DNS

udpBind(endpoint) returns Result<UdpSocket,Error>. receive(&mut destination[,deadline]) yields Datagram.bytes/peer/truncated; send(&source,peer[,deadline]) reports bytes sent. resolve(hostname,port[,deadline]) returns owning collections.List<Endpoint>; an overload selects family and transport explicitly. AddressFamily.Any is a DNS hint; explicit raw socket creation requires IPv4 or IPv6.

## Lifetime and errors

Deadline-free overloads use time.Deadline.noTimeout(). at(ticks) represents absolute monotonic nanoseconds; Deadline.after(nanoseconds) saturates at the latest finite value. time.after(Duration) instead returns Result and reports clock/overflow errors. Retries keep the same absolute deadline.

Socket and buffer loans survive pending operations until completion or confirmed cancellation. Full-duplex read/write is supported; simultaneous operations in the same direction can report Busy. Destruction and reactor shutdown preserve in-flight retention. Graph-owned sockets/buffers can belong to a spawned parent; borrowed operation children cannot be spawned independently.

Error records kind, domain, native code and transferred bytes. DNS List allocation failures report OutOfMemory/CapacityOverflow and destroy the temporary native answer owner. Raw reactor/native allocation contracts stay unchanged.

[net/raw](raw/README.md) exposes advanced socket operations. Platform layouts and the reactor ABI live under internal. TLS, HTTP and additional socket options are outside this stdlib package. See [the HTTP library and Web API example](../../examples/http_server/README.md) for an importable router built on TCP.
