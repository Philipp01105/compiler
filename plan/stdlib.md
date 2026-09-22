# Breiter Ausbau der DMM-stdlib

## Ziel und Leitlinien

Die stdlib erhält eine allgemeine Basis für Anwendungen sowie DMM-spezifische APIs für Ownership, Ressourcen und Async. Implementiert wird mit vorhandenen DMM-Sprachmitteln und OS-FFI. Neue Intrinsics oder Compiler-Sonderbehandlungen sind ausgeschlossen; nachgewiesene Compilerfehler dürfen korrigiert werden.

Bestehende APIs bleiben kompatibel. Neue Bereiche werden separat importiert, damit reine Text- und Collection-Anwendungen keine Threads oder Netzwerkimplementierungen benötigen.

## Umsetzung in acht Etappen

| Etappe | Packages | Inhalt |
|---|---|---|
| 1. Grundlagen | stdlib, stdlib/algorithms, stdlib/numeric, stdlib/binary | Option-/Result-Helfer; Suche, Sortierung, Vergleich und Slice-Operationen; geprüfte Ganzzahlarithmetik, Parsing und Formatierung; Endian-Konvertierung und begrenzte Binärreader/-writer |
| 2. Collections und Ownership | stdlib/collections, stdlib/memory | Bytes/List: reserve, append, clear, truncate, insert, remove; Deque, HashMap, HashSet; Box<T> und Byte-Arena mit besitzgebundenen Views |
| 3. Text und Unicode | stdlib/text, stdlib/unicode | StringBuilder, Suche, Split, Trim, Ersetzen; UTF-8/UTF-16-Konvertierung; Codepoints, Unicode-Klassifikation, Case-Mapping, Case-Folding und NFC/NFD/NFKC/NFKD |
| 4. Systemfunktionen | stdlib/path, stdlib/fs, stdlib/env, stdlib/time, stdlib/random, stdlib/math | Pfade, Dateien, Verzeichnisse, Metadaten, Umgebungsvariablen; Duration/Instant/SystemTime; Zufall und grundlegende Mathematik |
| 5. Synchronisation | stdlib/sync | Mutex<T> mit besitzendem Lock-Guard, begrenzte Channels mit Sender/Receiver und explizitem Schließen |
| 6. Async | stdlib/async | Nicht blockierende Timer, async Channel-Waits und Signal/Notification; gemeinsame Deadline-Typen für Timer und Netzwerk |
| 7. JSON | stdlib/json | Begrenzter Parser, besitzendes Document, geborgte Value-Views und Serializer |
| 8. HTTP | stdlib/http | HTTP/1.1-Parser/-Serializer, Async-Client und Server-Verbindungshandler über TCP; streamingfähige Bodies und Keep-Alive |

Jede Etappe wird mit Dokumentation, ausführbaren Beispielen und Tests abgeschlossen, bevor ihre Verbraucher entstehen.

## API- und Implementierungsregeln

- Neue fallible Konstruktoren und Operationen liefern Result<T,E>, optionale Treffer Option<T>. Fehler enthalten gegebenenfalls Byteposition oder nativen Fehlercode. Größenberechnungen werden vor Allocation geprüft.
- Bestehende kopierende Collections bleiben auf kopierbare Elemente beschränkt. HashMap/HashSet verwenden explizite Hash-/Vergleichsfunktionen und offene Adressierung. Box<T> besitzt und überträgt move-only-Payloads einzeln. Container mit move-only-Elementen benötigen allgemeine Heap-take/replace- und consuming-Option-Unterstützung; keine rohen Bytekopien initialisierter Ressourcen.
- Box<T> initialisiert und zerstört seinen Payload genau einmal und bietet checked get/getMut-Borrows. Die Arena verwaltet ausschließlich Bytes; Reset/Release wird bei lebenden Views abgelehnt. Keine allgemeine typisierte Arena ohne sichere Destruktionsregeln.
- Text unterscheidet Bytepositionen, Codepoints und Stringlängen. Binärdaten und eingebettete NULs verwenden Byte-Slices; OS-Pfade weisen eingebettete NULs zurück. Unicode-Daten werden auf 17.0.0 festgelegt, mit Lizenz und reproduzierbarem Generator eingecheckt. Sprachunabhängiges Case-Mapping ist enthalten, Collation und sprachabhängige Sonderregeln folgen später. Quelle: https://www.unicode.org/versions/Unicode17.0.0/
- Linux und Windows verwenden normale target-spezifische FFI-Dateien. Windows-Pfade verwenden UTF-16. Dateibesitzer schließen automatisch; explizites close meldet Fehler. Pfadoperationen bieten native und explizite POSIX-/Windows-Regeln.
- Ganzzahlparsing meldet Overflow und ungültige Eingaben. Zukünftige Float-Text-Konvertierung muss locale-unabhängig sein; Etappe 4 enthält Mathematik, keine Float-Text-API. Deterministischer PRNG und OS-Entropie sind getrennte APIs; der PRNG ist nicht kryptografisch.
- Guards binden Borrows an ihre Lebensdauer. Channels haben feste Kapazität, Backpressure, trySend/tryReceive und blockierende bzw. async Varianten. Move-only-Nachrichten verwenden Box-Handles. Close verhindert neue Nachrichten; eingereihte Nachrichten bleiben empfangbar.
- Timer und Channel-Waits parken Tasks über bestehende Waker-/Native-Future-Verträge. Keine blockierenden OS-Waits auf Executor-Workern oder Busy-Loops. Cancellation deregistriert Waits und bestätigt deren Ende vor Ressourcenfreigabe.
- JSON-Documents besitzen arena-basierte Knoten; Views borgen das Document. Zahlen behalten ihre lexikalische Darstellung und werden explizit konvertiert. UTF-8, Escapes und Surrogatpaare werden validiert. Limits für Eingabe, Tiefe und Knoten sind konfigurierbar; doppelte Objektkeys werden standardmäßig abgelehnt.
- HTTP-Framing folgt RFC 9112 (https://www.rfc-editor.org/rfc/rfc9112.html): Content-Length, Chunked und zulässige Close-delimited Responses. Mehrdeutiges Framing wird abgelehnt. Header-/Body-Limits und Deadlines sind explizit. Keine automatische Weiterleitung, Pipelining, TLS, HTTP/2 oder WebSocket; https liefert Unsupported.

## Zentrales Dokument für fehlende Sprachmittel

`plans/stdlib-language-gaps.md` sammelt belegte Lücken: betroffenes API, minimales Beispiel, Workaround und Sicherheits-/Ergonomiekosten, vorgeschlagenes allgemeines Sprachmittel und Einordnung als Compilerfehler, API-Blocker oder Vereinfachung.

Zuerst geprüft werden Moves aus Heap-Speicher, ein allgemeiner Copy-Constraint, capturing Closures und öffentliche Future-Poll-/Kompositionsmöglichkeiten. select, race, allgemeine Timeout-Wrapper und typisierte Task-Gruppen entstehen nur, wenn vorhandene Mittel Ownership und bestätigte Cancellation erhalten. Andernfalls dokumentieren wir konkrete Blocker statt unsicherer Ersatz-APIs.

## Tests und Abnahme

- Leere Eingaben, Grenzen, Overflow, Allocation-Fehler, Wachstum, Kollisionen, Teilübertragungen.
- Destruktionszähler/Rejection-Tests: einmalige Freigabe, ungültige Moves, lebende Borrows bei Mutation/Release.
- Versionierte Unicode-Konformitätstests, ungültiges UTF-8/UTF-16, Case-Mapping-Grenzfälle.
- Deterministische Concurrency-Tests: Backpressure, Close, Wake-Races, Timer, Cancellation.
- Fragmentiertes JSON/HTTP, ungültiges Framing, Limits, Streaming, Client-/Server-Loopback.
- Relevante Checks auf Windows/Linux bei O0/O1; beide Zielformate müssen kompilieren. Bestehende Tests und isolierte Installation aus stdlib-Quellpackages abschließend prüfen.

## Fortschritt

Stand der Sprachlücken-Erweiterung vom 5. Oktober 2026:
[plans/stdlib-language-gaps.md](../plans/stdlib-language-gaps.md) beschreibt die implementierten
Sprachmittel und verbleibenden Blocker; [tests/stdlib/README.md](../tests/stdlib/README.md)
ordnet ihnen die automatisierten Prüfungen zu. Der Gesamtplan ist weiterhin unvollständig.

- Etappe 1: implementiert. Option/Result unterstützen jetzt auch Owner-Payloads und consuming Mapper/Fallbacks. Slice-Algorithmen besitzen Copy-Bounds und akzeptieren explizite capturing Closures. Checked i64/u64, Parsing/Formatierung und Binärcursor sind vorhanden; gespeicherte checked Reader-/Writer-Adapter ergänzen die Cursor. Zusätzlich stdlib/limits: typisierte Min-/Max-Konstanten aller beschränkten skalaren Primitive, Float-/Double-Normal-/Subnormalgrenzen und Epsilon.
- Etappe 2: implementiert und erweitert. List, Deque und HashMap besitzen move-only Elemente; kopierende Zugriffe und Buffer/HashSet behalten ausdrückliche Copy-Bounds. Box<T> besitzt checked Borrows und intoInner überträgt den Payload; eine feste Byte-Arena ist vorhanden. Die Kapazitätsmethode heißt ensureCapacity, weil reserve ein Sprachkeyword ist. Wachstum und Allocation-Fehlerpfade sind mit Destruktionszählern geprüft. Die präzise Freigabe gespeicherter Container-Loans nach einzelnen Entfernungen bleibt offen.
- Etappe 3: implementiert. Textoperationen/StringBuilder, besitzgebundene move-only Split-/Codepoint-Iteratoren, UTF-8/UTF-16, Unicode-17-Klassifikation, volle Case-Mappings/Case-Folding und alle vier Normalformen. Unicode-Daten, Lizenz, Generator und offizielle Konformitätsvektoren eingecheckt.
- Etappe 4: implementiert. POSIX-/Windows-Pfade, besitzende Dateien/Directory-Iteratoren, Metadaten, Environment, monotone/Wall-Clocks, Duration und blockierendes Sleep, deterministischer PRNG/OS-Entropie, Double-Mathematik. Plattformaufrufe ausschließlich über normale FFI.
- Öffentliche Schnittstellen stehen in den gemeinsamen Package-Dateien; target-spezifische Dateien implementieren private Operationen. Alle stdlib-Imports verwenden import (...). Ein Import wie stdlib/fs wählt automatisch ELF/Linux oder COFF/Windows; Nutzer importieren keine Target-Dateien.
- Historische Abnahme der Grundetappen 1–4: Windows-Suite 57/57 und Linux-Suite 56/56 erfolgreich; danach betroffene Linux-Checks 11/11. O0/O1, Native/Intel/ATT/Object, ELF/COFF und isolierte Installation wurden damals geprüft. Unicode: 20.034 offizielle Normalisierungsvektoren mit jeweils 20 Invarianten, vier Case-Mappings für 3.037 gemappte Skalare und UTF-8-Roundtrips aller gültigen Skalare; Generator --check auf beiden Hosts erfolgreich. Diese Linux-Abnahme gilt nicht als Abschlussprüfung der aktuellen Sprachlücken-Erweiterung.
- Etappe 5: Mutex<T:Send> und owning, nicht-Send MutexGuard mit privaten Windows-/Linux-Implementierungen vorhanden; vollständige Channels bleiben ausstehend.
- Etappe 6: Poll, retained Waker, Context und Poller-Vertrag vorhanden. Öffentliches typisiertes Future-Polling, fromPoller, Komposition, nicht blockierende Timer und async Channel-Waits bleiben ausstehend.
- Etappen 7–8: JSON und HTTP ausstehend.
