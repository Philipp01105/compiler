# Voraussetzungen für `stdlib/net`

## Ziel

Der Meilenstein ist erreicht, wenn die Stdlib auf Windows und Linux TCP, UDP, DNS und asynchrone I/O implementieren kann. Dieser Plan liefert Sprach- und Runtime-Grundlagen sowie eine kleine `stdlib/core/net`-Schnittstelle; die nutzerfreundliche `stdlib/net`-API, HTTP und TLS folgen danach. Programme ohne Netzwerk bleiben wie bisher standalone.

## Umsetzung

1. **Async- und Concurrency-Regeln festlegen und implementieren.** `async func (…) -> T` erzeugt beim Aufruf einen move-only `Future<T>`; `await` liefert `T` und ist nur in asynchronen Funktionen zulässig. Compiler, IR und Backend senken Futures zu zustandsbasierten, heap-allokierten Frames ab. Der Frame bleibt gepinnt, auch wenn sein Future-Handle bewegt wird. Der Borrow-Checker erlaubt Referenzen über `await`, solange ihr Besitzer gültig und gepinnt bleibt; geliehene Parameter binden die Lebensdauer des Futures an ihren Ursprung. Ein Task, der zwischen Executor-Threads wandern darf, muss vollständig `Send` sein. `Send`/`Sync` werden konservativ aus Feldern und Referenzen abgeleitet; rohe Zeiger erhalten sie nicht automatisch. Dazu gehören dokumentierte atomare Operationen und ein Speichermodell.

2. **Executor und Abbruchsemantik bereitstellen.** Ein Multi-Thread-Executor bietet `spawn(Future<T>) -> JoinHandle<T>` und `block_on(Future<T>) -> T`. Es gibt zunächst keine losgelösten Tasks. Ein gestarteter Future oder Join-Handle muss auf jedem Pfad awaited oder ausdrücklich abgebrochen werden; implizites Drop einer laufenden Operation ist ein Fehler. `cancel(handle)` liefert eine awaitbare Abschlussoperation. Frame, geliehener Puffer und OS-Handle bleiben bis zur bestätigten Beendigung ausstehender I/O erhalten; Cleanup und Destruktoren laufen genau einmal.

3. **Gezielten externen Linker-Handoff bauen.** Implementiert ist der private Plattform-Handoff: IR-Runtime-Anforderungen, Runtime-Profil und Linkstrategie sind getrennt. `--link=external` fordert vorerst explizit das Plattform-Profil an, keine Netzwerk-Anforderung. Objekt- und Assembly-Ausgabe verwenden dessen privaten ABI ohne Linkprozess; Executables linken den separaten Shim über einen GCC-kompatiblen Treiber. Der Shim verwendet Linux-pthreads oder Windows-UCRT64 `_beginthreadex` und Plattform-Startup; die generierte `__dmm_runtime_main`-Brücke besitzt den gesamten DMM-Lifecycle. `NETWORK` ist für Schritt 4 reserviert und wird später nur durch benötigte Netzwerk-Intrinsics ausgelöst. Der Standalone-Pfad bleibt unverändert. Vertrag und manuelle Linkbefehle stehen in [NATIVE_BACKEND.md](NATIVE_BACKEND.md). Allgemeine öffentliche C-Interop und statische Bibliotheksverwaltung sind dafür keine Voraussetzung.

4. **Plattform-Runtime und Core-Basis liefern.** Der private Shim kapselt `epoll` auf Linux und IOCP auf Windows, TCP-/UDP-Sockets, IPv4/IPv6-Adressen, nichtblockierende Operationen, monotone Timeouts und plattformspezifische Fehler. DNS läuft über einen begrenzten Worker-Pool, sodass Auflösung den Executor nicht blockiert. Windows-Netzwerkinitialisierung ist thread-sicher und endet erst nach allen Tasks. `stdlib/core/net` stellt darauf nur typisierte, niedrigstufige Operationen und einen move-only Socket-Besitzer bereit; die eigentliche öffentliche `stdlib/net`-Gestaltung beginnt nach diesem Meilenstein.

## Prüfung und Abnahmekriterium

- Linux- und Windows-CI prüfen Objekt- und Assembly-Pfad, Optimierungsstufen und den externen Linker. Bestehende Standalone-Tests müssen unverändert bestehen; nur Netzprogramme dürfen zusätzliche Systemabhängigkeiten haben.
- Compiler-Tests prüfen `async/await`, Pinning, Borrows über Suspendierungen, `Send`/`Sync`, alle Ownership-Pfade und die Ablehnung implizit fallengelassener laufender Futures.
- Loopback-Tests decken TCP-Client/Server, UDP, IPv4/IPv6, partielle Übertragungen, asynchrones DNS, Timeout und Abbruch während ausstehender I/O ab. DNS-Fehler- und Race-Tests verwenden zusätzlich einen kontrollierbaren Resolver statt Internetzugang.
- **Fertig**, sobald ein DMM-Testprogramm ausschließlich mit `stdlib/core/net` und dem Executor diese Szenarien sicher ausführen kann. Erst dann wird die höhere `stdlib/net`-API implementiert.

## Festgelegte Grenzen

Kein `async/await`-Syntax-Sonderweg nur für Networking: Die Sprachfunktion ist allgemein nutzbar. Kein TLS, HTTP, detached Tasks oder allgemeiner C-FFI-Ausbau in diesem Meilenstein. Der Plan ersetzt den pauschalen TODO „Concurrency später“ durch diese gezielten Vorarbeiten und ergänzt einen eigenen TODO für die Socket-/Reactor-Basis.
