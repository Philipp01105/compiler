# Native FFI und Runtime-Migration nach DMM

## Stand und Umfang

Etappen 1–4 sind implementiert und geprüft. Die allgemeine FFI ist seit Etappe 2B eigenständig nutzbar; Etappe 3
ergänzt die Fähigkeiten für Runtime-Code. Die Plattform-Runtime liegt seit Etappe 4 in DMM. **Etappe 5 bleibt offen:**
Die Netzwerkimplementierung ist weiterhin in C und wird zusammen mit dem DMM-Plattformobjekt installiert.

Targets sind x86-64 Linux/glibc und Windows/MinGW-w64 UCRT64, mit GCC oder Clang als externem Linktreiber.
libc/UCRT, OS-Bibliotheken und reguläres Plattform-Startup bleiben Abhängigkeiten. Nach Etappe 5 soll kein eigener
Plattform- oder Netzwerk-C-Shim mehr benötigt werden. TLS-Protokoll, HTTP, C++, Bindgen, Variadics, dynamisches
Nachladen, Closure-Trampolines und fremdes Unwinding sind außerhalb dieses Vorhabens.

Der verbindliche Sprachvertrag steht in der [Sprachspezifikation](../LANGUAGE_SPEC.md#native-ffi), die technischen
Details in der [Architektur](../ARCHITECTURE.md#native-x86-64-backend). [Raw-Bindings](../stdlib/native/README.md)
und die [Netzwerkverträge](../stdlib/core/net/README.md) liegen bei ihren Packages.

## Feste Architekturentscheidungen

```dmm
package winsock;

extern "system" {
    pub struct NativeHandle;
    pub struct SOCKADDR {
        pub var family:u16;
        pub var data:u8[14];
    }
}

extern "system" from "ws2_32" {
    pub func lastError() -> i32 = "WSAGetLastError";
    pub func closeSocket(socket:usize) -> i32 = "closesocket";
}
```

- `extern "system"` verwendet die ABI des Ausgabe-Targets. `from` ist eine logische Library-ID, etwa `c`, `pthread`
  oder `ws2_32`; es beeinflusst Typdeklarationen nicht. Aliase gehören zum Importvertrag.
- Native POD-Typen verwenden echtes Target-C-Layout. Gemeinsame Layoutberechnung liefert Größe, Alignment,
  Feldoffsets und Array-Strides für Sema, Metadaten, IR und Backend. Native Werte sind copyable und besitzen keine
  Drop-Glue. Unvollständige/by-value-rekursive Typen sind ausgeschlossen, Pointer-Rekursion ist erlaubt.
- Gewöhnliche DMM-Aggregate behalten ihre acht Byte großen Slots. Native Werte benötigen bytegenaue Zugriffe und
  Kopien auch innerhalb solcher Aggregate und Arrays. Die native ABI-Klassifikation bleibt von der DMM-ABI getrennt.
- FFI-Werte sind feste Integerbreiten, `isize`, `usize`, `float`, `double`, `bit`, rohe Pointer und vollständige native
  Aggregate. `bit` bildet C `_Bool` ab; Win32 `BOOL` wird als `i32` deklariert. `int`, `char`, `byte`, Strings,
  Slices, checked References, Interfaces, Enums, Futures und gewöhnliche DMM-Aggregate passieren die Grenze nicht
  by value. Feste Arrays sind native Felder; `void` ist Rückgabetyp oder Pointer-Pointee. Kein implizites Marshaling.
- Native Deklarationen sind eine Vertrauensgrenze ohne neue `unsafe`-Syntax. Bindings verantworten Signaturen,
  Layout, Pointergültigkeit, Alignment, Ownership, Lifetimes und Synchronisation. Eine falsche Deklaration kann
  Memory-Safety-Garantien aufheben. Pointer auf gewöhnliche DMM-Typen sind nur opaque Adressen; ein expliziter Cast
  konvertiert oder validiert kein Layout. Callback-Kontexte leben bis zum bestätigten Ende aller nativen Aufrufe.
- Package-Sichtbarkeit folgt den normalen Regeln. Opaque Structs sind ausschließlich hinter Pointern verwendbar.
  Native Typen haben keine Methoden, Generics, Destruktoren oder Feldinitialisierer.

## Etappe 1: Frontend, Typen und IR — abgeschlossen

Lexer, Parser, Symbolauflösung und IDE-Analyse unterstützen Extern-Blöcke, bodylose Funktionen, Aliase und
opaque/native Structs. Das Target-Modell steht vor Sema fest. Native Imports sind separate IR-Deklarationen mit
Symbol-ID, ABI, logischer Library, nativem Namen, Signatur und Source-Span; sie erhalten keine künstlichen DMM-Bodies.
Der Verifier prüft Layouts, Signaturen und Call-Referenzen. Illegale Deklarationen erhalten Quelldiagnosen.

Prüfung: Frontend-/IDE-Fälle und C-Referenzen für Größe, Alignment und Feldoffsets.

## Etappe 2A: Skalare Calls und Linking — abgeschlossen

Integer, Floating Point, Pointer und `void` verwenden die native Call-ABI. Kleine Rückgaben werden aus ihrer
definierten Breite normalisiert. Argumente werden genau einmal in Quellreihenfolge ausgewertet; native Calls gelten
als potenziell speicherverändernd.

Nur tatsächlich emittierte Imports und Funktionsadressen erzeugen Library-Anforderungen. Sie wählen das
Plattform-Profil und bei `auto` den externen Linker; `internal` wird mit konkretem Grund abgelehnt. Logische IDs werden
zu separaten `-lNAME`-Argumenten. `--native-library NAME=PATH` überschreibt eine ID mit einer Datei;
`--native-library-dir DIR` ist wiederholbar. Dateinamen und beliebige Linkeroptionen gehören nicht in `from`.
Widersprüchliche Signaturen oder Library-Zuordnungen desselben nativen Symbols werden zurückgewiesen.

GCC/Clang werden ohne Shell mit dynamischen Argumentlisten gestartet. Der Linkprozess arbeitet mit einer temporären
Ausgabe und veröffentlicht erst bei Erfolg; bestehende Programme bleiben bei Fehlern erhalten. `-c` und `-S` starten
keinen Linker. `--dump-native-link FILE` beschreibt Target, Profil und benötigte Libraries für manuelles Linking.

Prüfung: kleine signierte/unsigned Rückgaben, `_Bool`, gemischte Argumente, Registererschöpfung, Aliase,
`getpid`/`GetCurrentProcessId`, Overrides, Pfade mit Leerzeichen, ungenutzte Imports, fehlende Dateien/Symbole,
Cross-Target-Emission und Erhalt vorhandener Ausgaben bei Fehlern.

## Etappe 2B: Aggregate-ABI — abgeschlossen, FFI V1

System V klassifiziert INTEGER-/SSE-Eightbytes, Register- und Speicherübergabe, atomaren Register-Rollback bei
Erschöpfung und Hidden-Result-Pointer. Windows verwendet positionsabhängige Register, Shadow Space, ausgerichtete
Aggregatkopien und Hidden-Result-Pointer. `native-copy` sichert by-value-Argumente während ihrer Auswertung, sodass
spätere Mutationen bereits erfasste Werte nicht verändern.

Prüfung: kleine/verschachtelte Structs, gemischte INTEGER-/SSE-Rückgaben, Stackübergabe, Padding und native Werte in
DMM-Aggregaten/Arrays. Ein Drei-Byte-Struct direkt vor einer Schutzseite prüft bytegenaue Kopien. GCC und Clang
prüfen Ausführung bei `-O0` und `-O1` auf beiden Targets.

Referenzen: [AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/raw/master/x86-64-ABI/low-level-sys-info.tex),
[Microsoft x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170),
[Windows-Typdefinitionen](https://learn.microsoft.com/en-us/windows/win32/winprog/windows-data-types).

## Etappe 3: Runtime-fähige FFI — abgeschlossen

Native Function-Pointer-Typen `extern "system" func(...) -> T` und stabile Einstiegspunkte
`export "system" func name(...) -> T { ... }` verwenden denselben ABI-Klassifikator für ein- und ausgehende Calls.
Exports sind synchron, nicht generisch und FFI-sicher. Native und gewöhnliche DMM-Callable-Typen bleiben verschieden.
Windows-Einstiegspunkte und synchrone Plattform-Bodies erhalten Unwind-Metadaten einschließlich großer Stackframes.
Fremde Exceptions dürfen trotzdem nicht durch DMM-Frames unwinden.

Native Unions, `pack(N)` und `align(N)` unterstützen 1, 2, 4, 8 und 16. Layout und Klassifikation berücksichtigen
überlappende und unaligned Felder. `_linux.dmm`/`_windows.dmm` werden vor dem Parsen anhand des Ausgabe-Targets
ausgewählt; Manifest-Synchronisierung berücksichtigt beide Varianten. Raw-Bindings für glibc, pthreads, Kernel32,
UCRT und Winsock stellen die für epoll, IOCP und DNS benötigten APIs und Typen bereit.

Prüfung: indirekte Calls, skalare/Aggregat-Callbacks, Hidden-Results, Union/Packing/Alignment in allen Speicherorten,
vier parallele native Threads mit bestätigtem Join und Context-Lifetime, Windows-Unwind/Stackframes, C-Header-Layouts
und echte pthread-, epoll/eventfd-, Event-, IOCP-, Winsock- und DNS-Aufrufe. ELF/COFF-Bindings werden auch vom jeweils
anderen Host emittiert; GCC/Clang und beide Assembly-Syntaxen sind geprüft.

## Etappe 4: Plattform-Runtime in DMM — abgeschlossen

Threadstart, Join, manuelle Reset-Events und Prozessbeendigung liegen in
[`src/runtime/platform`](../src/runtime/platform). Private Symbole und Ownership-Verträge bleiben erhalten.
Linux verwendet pthreads mit libc-TLS, Mutex-Predicate und Condition-Variable-Schleife. Windows verwendet
`_beginthreadex`, normale Callback-Rückkehr, bestätigtes Join und `CloseHandle`. Context-Speicher wird erst nach
Thread-Ende freigegeben; Ressourcen/API-Fehler bleiben fatal.

Der Compiler erzeugt `main` als native ABI-Brücke zu `__dmm_runtime_main`. Reguläres OS-/CRT-Startup initialisiert
den Prozess; Package-Init, DMM-main, Executor-Drain, Cleanup und Exitcode bleiben im generierten Ablauf.

`--runtime-component` verlangt ein Library-Package und emittiert ein Objekt, optional Assembly. Es erlaubt nur die
festgelegten privaten Plattform-Exports und den Thread-Einstieg; DMM-Helfer bleiben lokal. Application-Startup,
automatische Runtime-Verknüpfung, implizite Basishelfer, erreichbare Async-/Drop-Bodies und dynamische globale
Initialisierung/Cleanup sind ausgeschlossen. Explizite native Abhängigkeiten stehen im Linkinventar mit Profil
`component`. So lädt die Komponente niemals ihre eigene Runtime nach und erzeugt keinen Bootstrap-Zyklus.

CMake baut zuerst den Compiler und dann damit das Plattformobjekt. Der Name `platform-shim.o` bleibt zur
Kompatibilität bestehen, enthält aber ausschließlich DMM-Code. Installation enthält Objekt und Linkinventar;
Default-Build oder Targets `dmm_platform_runtime`/`dmm_network_runtime` müssen vor Installation gebaut sein.
`network-shim.a` bündelt vorerst das verbleibende Netzwerk-C-Objekt mit dem DMM-Plattformobjekt. GNU-/LLVM-Archive
sind unterstützt; der Linker prüft das Target jedes Members und verknüpft nur das benötigte Bundle.
Die Plattform-C-Implementierung ist entfernt; `platform_shim.h` bleibt als private ABI-Deklaration für Netzwerkcode
und unabhängige Vertragsprüfungen.

Prüfung: Bootstrap ohne rekursive Abhängigkeiten, ELF/COFF, `-O0`/`-O1`, beide Assembly-Syntaxen, TLS/errno,
parallele Threads, Join, Signal vor Wait, wiederholtes Wait ohne Reset, Broadcast, Worker-Exit, Startup, Shutdown und
Netzwerk-Lifecycle. Vollständige Etappe-4-Suiten bestanden auf Linux und Windows; frische Clang-Builds, GNU-/LLVM-
Archive, installierte Compiler und paralleler Neubau bestätigten Build- und Installationsverträge.

## Etappe 5: Netzwerk-Runtime in DMM — offen

1. Gemeinsame Socket-/Operation-Besitzer, Fehlerübersetzung, Deadlines und Lifecycle portieren.
2. Linux-Reactor mit epoll/eventfd und Windows-Reactor mit IOCP/Overlapped implementieren.
3. DNS-Queue und Resolver-Worker portieren.
4. C- und DMM-Implementierung übergangsweise per Build-Option auswählbar halten. Nach bestätigter Parität nur DMM
   installieren und Netzwerk-C-Shims entfernen.

Die private Netzwerkschnittstelle bleibt Integrationsgrenze; die bestehende `stdlib/core/net`-Source-API bleibt
erhalten. Verbindlich bleiben Read-/Write-Slots, stabile Operation-Speicher, einmalige Veröffentlichung, Wake außerhalb
von Locks, bestätigte Cancellation, keine vorzeitige Bufferfreigabe und begrenzte DNS-Admission.
Lifecycle: Executor-Drain bei verfügbarem Netzwerk → DRAINING → Package-Cleanup → bestätigter Reactor-/Resolver-Abschluss.

Abnahme: bestehende TCP/UDP-, IPv4/IPv6-, DNS-, Deadline-, Cancellation-, Race- und Lifecycle-Szenarien laufen ohne
eigenen Plattform-/Netzwerk-C-Code. Native ABI, Ownership, Async und Atomics bleiben Compileraufgaben;
Socket-, epoll-, IOCP- und DNS-Implementierungen liegen in DMM-Libraries.

Dokumentation wird nur um tatsächlich abgeschlossene Fähigkeiten aktualisiert. Erst diese Abnahme schließt das
Gesamtvorhaben ab.
