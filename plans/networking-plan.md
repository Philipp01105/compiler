# Voraussetzungen für `stdlib/net`

Stand: Die Grundlagen dieses Plans sind mit `c92aaa8` implementiert. `stdlib/core/net` liefert TCP/UDP, IPv4/IPv6,
monotone Deadlines, bestätigten Abbruch und begrenztes DNS. Die normative Beschreibung steht in
[NETWORK_RUNTIME.md](../NETWORK_RUNTIME.md); Async- und Ownership-Regeln stehen in
[LANGUAGE_SPEC.md](../LANGUAGE_SPEC.md). Die öffentliche `stdlib/net`-API, HTTP und TLS bleiben Folgearbeiten.

## Ziel

Der Meilenstein ist erreicht, wenn die Stdlib auf Windows und Linux TCP, UDP, DNS und asynchrone I/O implementieren kann. Dieser Plan liefert Sprach- und Runtime-Grundlagen sowie eine kleine `stdlib/core/net`-Schnittstelle; die nutzerfreundliche `stdlib/net`-API, HTTP und TLS folgen danach. Programme ohne Netzwerk bleiben wie bisher standalone.

## Umsetzung

1. **Async- und Concurrency-Regeln — implementiert.** Die Root-Manifest-Option `features = ["async"]` aktiviert `async func (…) -> T`, dessen Aufruf einen lazy, move-only `Future<T>` erzeugt. `future.await()` konsumiert den Future und liefert `T`; diese Form ist nur in asynchronen Funktionen zulässig, Prefix-`await` wird abgelehnt. Compiler, IR und Backend senken Futures zu zustandsbasierten, heap-allokierten Frames ab. Der Frame bleibt gepinnt, auch wenn sein Future-Handle bewegt wird. Geliehene Parameter binden die Lebensdauer des Futures an ihren Ursprung. Spawn verlangt einen vollständig `Send`-fähigen Frame und Output. Frame-interne Loans sind bei nachgewiesenem gepinntem Besitzergraph erlaubt, machen den geliehenen Child-Future aber nicht unabhängig spawnbar. `Send`/`Sync` werden konservativ abgeleitet; `AtomicBit` und `AtomicUsize` bieten sequenziell konsistente Operationen.

2. **Executor und Abbruchsemantik — implementiert.** Multi-Thread-Executors bieten `spawn(Future<T>) -> JoinHandle<T>` und synchrones `block_on(Future<T>) -> T`. Es gibt keine losgelösten Tasks. Futures, Join-Handles und Executor-Besitzer müssen auf jedem Pfad konsumiert oder weitergegeben werden. `handle.await()` liefert `Result<T,TaskError>`; `cancel(future)` und `cancel(handle)` liefern eine zu konsumierende Abschlussoperation. `executor.shutdown(ShutdownMode.Drain|Cancel)` konsumiert den Executor und liefert `Future<void>`. Frame, geliehener Puffer und OS-Handle bleiben bis zur bestätigten Beendigung ausstehender I/O erhalten; aktive Defers und Destruktoren laufen genau einmal. Der Vertrag steht in [src/runtime/EXECUTOR.md](../src/runtime/EXECUTOR.md).

3. **Gezielter externer Linker-Handoff — implementiert.** IR-Runtime-Anforderungen, Runtime-Profil und Linkstrategie sind getrennt. Verwendete Netzwerk-Intrinsics lösen `NETWORK` und damit `PLATFORM_RUNTIME` aus; `--link=auto` linkt solche Executables extern, `--link=internal` lehnt sie ab. Ein ungenutzter Import aktiviert keine Netzwerkabhängigkeit. `--link=external` wählt auch ohne Netzwerk das Plattform-Profil. Objekt- und Assembly-Ausgabe verwenden dessen privaten ABI ohne Linkprozess. Der GCC-kompatible Treiber bindet den Plattform-Shim oder den kombinierten Netzwerk-Shim ein. Dieser verwendet Linux-pthreads oder Windows-UCRT64 `_beginthreadex`; die generierte `__dmm_runtime_main`-Brücke besitzt den DMM-Lifecycle. Vertrag und manuelle Linkbefehle stehen in [NATIVE_BACKEND.md](../NATIVE_BACKEND.md). Allgemeine öffentliche C-Interop und statische Bibliotheksverwaltung sind dafür keine Voraussetzung.

4. **Plattform-Runtime und Core-Basis — implementiert.** Der private Shim kapselt `epoll` auf Linux und IOCP auf Windows, TCP-/UDP-Sockets, IPv4/IPv6-Adressen, nichtblockierende Operationen, monotone Deadlines und typisierte Fehler. DNS läuft über zwei lazy Worker mit höchstens 64 wartenden Jobs. `stdlib/core/net` stellt typisierte Operationen sowie move-only Socket- und Adresslisten-Besitzer bereit. Netzwerk bleibt während des Executor-Drain verfügbar, wechselt vor Package-Cleanup zu DRAINING und endet nach Cleanup und bestätigter Beendigung aller Runtime-Arbeiten. Öffentliche `stdlib/net`-Gestaltung folgt danach.

## Prüfung und Abnahmekriterium

- Linux- und Windows-CI prüfen Objekt- und Assembly-Pfad, Optimierungsstufen und den externen Linker. Bestehende Standalone-Tests müssen unverändert bestehen; zusätzliche Systemabhängigkeiten gehören zum expliziten Plattform-Profil oder zu Netzprogrammen.
- Compiler-Tests prüfen `async/await`, Pinning, Borrows über Suspendierungen, `Send`/`Sync`, alle Ownership-Pfade und die Ablehnung implizit fallengelassener laufender Futures.
- Loopback-Tests decken TCP-Client/Server, UDP, IPv4/IPv6, partielle Übertragungen, asynchrones DNS, Timeout und Abbruch während ausstehender I/O ab. DNS-Fehler- und Race-Tests verwenden zusätzlich einen kontrollierbaren Resolver statt Internetzugang.
- Die vorhandenen Verträge `async_frontend_contract`, `async_runtime_contract`, `async_executor_contract`, `platform_handoff_contract`, `network_shim_unit` und `network_runtime_contract` decken diese Grundlagen ab. Runnable DMM-Szenarien stehen in [loopback.dmm](../tests/network/loopback.dmm) und [lifecycle.dmm](../tests/network/lifecycle.dmm). Die höhere `stdlib/net`-API bleibt der nächste eigenständige Meilenstein.

## Festgelegte Grenzen

Kein `async/await`-Syntax-Sonderweg nur für Networking: Die Sprachfunktion ist allgemein nutzbar. Kein TLS, HTTP, detached Tasks oder allgemeiner C-FFI-Ausbau in diesem Meilenstein. Der Plan ersetzt den pauschalen TODO „Concurrency später“ durch diese gezielten Vorarbeiten und ergänzt einen eigenen TODO für die Socket-/Reactor-Basis.
