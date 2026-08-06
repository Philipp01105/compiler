# Native FFI und Runtime-Migration nach DMM

## Status und Ziel

Etappen 1, 2A und 2B sind implementiert: native Deklarationen, gemeinsame
C-Layouts, überprüfbare Import-IR, skalare Calls, Structs by value und externer
Linkpfad. FFI V1 ist eigenständig nutzbar. Der Implementierungs- und Prüfstand
von Etappe 2 ist in [ffi-stage2.md](ffi-stage2.md) festgehalten.

Etappe 3 ist ebenfalls implementiert und geprüft: native Function-Pointer,
Exports und Callbacks, Unions und Packing/Alignment, Windows-Unwind-Metadaten,
Target-Dateiauswahl und Raw-OS-Bindings. Der Prüfstand steht in
[ffi-stage3.md](ffi-stage3.md).

Etappe 4 ist implementiert und geprüft: Plattform-Threads, Events, Join und Exit
liegen in DMM; der Compiler erzeugt die Startup-Brücke. Der Bootstrap-Modus baut
und installiert die Komponente ohne rekursive Runtime-Einbindung. Eigener
Plattform-C-Code ist entfernt. Der Prüfstand steht in [ffi-stage4.md](ffi-stage4.md).
Nur Etappe 5 bleibt offen; die Netzwerkimplementierung ist weiterhin in C.

DMM erhält eine allgemeine native FFI für x86-64 Linux/glibc und Windows mit
MinGW-w64 UCRT64. GCC und Clang dienen als externe Treiber. Danach werden die
Plattform- und Netzwerk-Runtime vollständig nach DMM portiert. libc/UCRT,
OS-Bibliotheken und reguläres Plattform-Startup bleiben Abhängigkeiten;
eigene C-Shims entfallen nach bestätigter Verhaltensgleichheit.

Nicht enthalten: TLS-Protokoll, HTTP, C++, Bindgen, dynamisches Nachladen,
Variadics, Closure-Trampolines oder fremdes Unwinding durch DMM-Frames.

## Sprach- und Speichervertrag

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

- Nur `extern "system"`; das Ausgabe-Target bestimmt die ABI.
- `from` bezeichnet eine **logische Native-Library-ID**, keine DLL, SONAME,
  Dateipfade oder beliebige Linkeroptionen. IDs beginnen mit einem Buchstaben
  oder Unterstrich; danach folgen Buchstaben, Ziffern, Unterstriche oder Bindestriche.
  Beispiele: `c`, `m`, `pthread`, `kernel32`, `ucrt`, `ws2_32`.
- `from` hat **keinen Einfluss auf Typdeklarationen**. NativeType besitzt keine
  Library-Abhängigkeit; Layoutabfragen benötigen weder Library-Datei noch Linker.
- Native Funktionen haben eine vollständige Signatur, keinen Body und optional
  einen Alias `= "NativeName"`. Aliase sind C-Identifier und Teil von V1.
  Ohne `from` sind nur Typdeklarationen erlaubt.
- Package-Namen und `pub` folgen den bestehenden Regeln. Keine neue `private`-
  oder `unsafe`-Syntax. Native Felder verwenden `pub var field:type;` beziehungsweise
  `var field:type;`. Keine Methoden, Generics, Destruktoren oder Initialisierer.
- Opaque `struct NativeHandle;` ist nur hinter rohen Pointern verwendbar.
- FFI-Werte: feste Integerbreiten, `isize`, `usize`, `float`, `double`, `bit`,
  rohe Pointer und vollständige native POD-Structs. `int`, `char` und `byte`
  sind als FFI-Werte ausgeschlossen; Bindings verwenden explizite Breiten.
  `void` ist nur Rückgabetyp oder Pointer-Pointee. Feste Arrays sind native
  Felder, keine unmittelbaren Parameter oder Rückgabewerte.
- `bit` besitzt eine Ein-Byte-Repräsentation mit 0/1-Semantik und bildet C `_Bool`
  ab. Win32 `BOOL` wird als `i32` deklariert.
- Keine Strings, Slices, checked References, Interfaces, Enums, Futures,
  gewöhnlichen DMM-Aggregate oder implizites Marshaling by value.
- Eine gemeinsame Layoutberechnung liefert Größe, Alignment, Feldoffsets und
  native Array-Strides. Native Structs haben internes und abschließendes
  C-Padding, sind copyable und besitzen keine Drop-Glue. By-value-Zyklen,
  leere native Structs und unvollständige Werttypen werden abgelehnt;
  Pointer-Rekursion ist erlaubt.
- DMM-Aggregate behalten ihre acht Byte großen Slots. Native Werte benötigen
  bytegenaue Speicheroperationen, auch in DMM-Aggregaten und Arrays.

### Vertrauensgrenze und rohe Pointer

Die Binding-Deklaration behauptet, dass Signatur und Datenlayout das native
Symbol korrekt beschreiben. Native Funktionen sind nicht automatisch
memory-safe. Eine falsche Deklaration oder ungültige Pointer können die normalen
Memory-Safety-Garantien aufheben. Binding-Packages verantworten Pointergültigkeit,
Alignment, Lifetimes, Ownership, Fehlercodes und Thread-Sicherheit.

Pointer auf gewöhnliche DMM-Typen dürfen als opaque Kontextadressen passieren;
dies garantiert **kein natives Pointee-Layout**. Referenzen werden ausdrücklich
in rohe Pointer gecastet:

```dmm
extern "system" from "example" {
    func saveContext(context:*void) -> void;
}

struct Context { var value:i32; }

func registerContext() -> void {
    var context:Context;
    saveContext((&context).(*void));
    // Nur korrekt, wenn saveContext den Pointer nicht über diesen Scope hinaus nutzt.
}
```

Ein Cast von `*Context` auf `*NativeType` reinterpretieriert ausschließlich die
Adresse. Er konvertiert oder validiert kein Layout. Native C-Funktionen dürfen
DMM-Speicher nicht allein aufgrund eines solchen Casts als C-Struct interpretieren.

## Etappe 1: Frontend, native Typen und überprüfbare IR

- Lexer, Parser, Symbolauflösung und IDE-Analyse unterstützen Extern-Blöcke,
  bodylose Funktionen, Aliase und opaque/native Structs.
- Das Target-Modell wird vor Sema festgelegt. Sema, Layoutabfragen und IR verwenden
  dieselbe native Layoutberechnung.
- NativeImports sind separate IR-Deklarationen: Symbol-ID, ABI, logische Library,
  nativer Name, Signatur und Source-Span. Keine künstlichen DMM-Funktionsbodies.
- Native Layoutinformationen einschließlich Feld-Offsets und Array-Strides gehen
  in IR ein. Der Verifier prüft Deklarationen, Layout, Signaturen und Calls.
- AST-/IR-Dumps erhalten Version 4. Function-Pointer-Werte bleiben bis Etappe 3
  ausgeschlossen; direkte Calls sind bereits semantisch analysierbar.

Abnahme: Alle nativen Deklarationen sind analysierbar; illegale Signaturen und
Layouts werden mit Quellposition zurückgewiesen. C-Referenztypen prüfen Größe,
Alignment und Feldoffsets. Native Speicheroperationen werden erst in Etappe 2
freigegeben.

## Etappe 2A: Skalare native Calls und externer Linker

- Native ABI-Klassifikation für Integer, Floating Point, rohe Pointer und `void`;
  korrekte Erweiterung kleiner Rückgaben aus ihrer definierten Breite.
- Argumente werden genau einmal in bestehender Reihenfolge ausgewertet.
  Native Calls gelten als potenziell speicherverändernd.
- Libraries aus tatsächlich emittierten Calls und später Funktionsadressen
  sammeln. Ungenutzte Imports und reine Typdeklarationen erzeugen keine Abhängigkeit.
- Verwendete Imports wählen das Plattform-Profil und bei `auto` den externen
  Linker. `internal` wird mit konkreter Begründung abgelehnt.
- Dynamische Argumentlisten ohne Shell. Der Target-Link-Layer löst logische IDs
  auf: etwa `ws2_32` zu `-lws2_32`, `c` zur C-Library des Treibers.
- `--native-library NAME=PATH` überschreibt eine ID mit einer expliziten
  Linkerdatei; wiederholbares `--native-library-dir DIR` ergänzt Suchpfade.
  Explizite SONAME-Dateien werden über Overrides angegeben, nicht in `from`.
- `-c` und `-S` starten keinen Linker. Optionales `--dump-native-link FILE`
  beschreibt Target, Runtime-Profil und benötigte Libraries für manuelles Linking.
- Gleiche native Symbolnamen mit unterschiedlichen Library-Zuordnungen oder
  widersprüchlichen Signaturen werden mit Source-Spans abgelehnt.
- Fehlende Dateien und Linkfehler erhalten klare Diagnosen. Bestehende
  Ausgabeprogramme bleiben bei Fehlern erhalten.

Abnahme: Scalar-ABI-Fälle, Aliase, reale Plattformfunktionen, Linkerfehler,
Cross-Target-Emission und ungenutzte Imports unter GCC und Clang.

## Etappe 2B: Native Aggregate-ABI

Eigene native ABI-Klassifikation, unabhängig von DMM-Aggregatübergabe:

- System V: Eightbytes, INTEGER-/SSE-Klassen, Registererschöpfung mit atomarem
  Rückfall eines Aggregats auf Speicher, Stack-Alignment und Hidden-Result-Pointer.
- Windows x64: positionsabhängige Register, Shadow Space, native Aggregatkopien
  und Hidden-Result-Pointer.
- Bytegenaue lokale Werte, Kopien, Feldzugriffe und Arrays; verschachtelte
  native Structs in gewöhnlichen DMM-Aggregaten.

Abnahme: Kleine C-Testbibliotheken für Struct-Argumente und Struct-Rückgaben,
gemischte INTEGER/SSE-Klassen, Register- und Speicherübergabe, Padding,
verschachtelte Structs und Hidden-Result-Pointer auf beiden Targets.
**Erst nach dieser Abnahme ist FFI V1 abgeschlossen.**

Referenzen: [AMD64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/raw/master/x86-64-ABI/low-level-sys-info.tex),
[Microsoft x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170),
[Windows-Typdefinitionen](https://learn.microsoft.com/en-us/windows/win32/winprog/windows-data-types).

## Etappe 3: Voraussetzungen für die Runtime-Migration

- Native Function-Pointer-Typen: `extern "system" func(...) -> T`.
- Native Einstiegspunkte: `export "system" func name(...) -> T { ... }`.
  Derselbe Klassifikator bedient eingehende und ausgehende Calls.
- Exports sind synchron, nicht generisch und FFI-sicher. Keine implizite
  Konversion gewöhnlicher DMM-Callbacks, keine Closure-Trampolines.
- Stabile Callback-Adressen. Kontexte und Ressourcen leben bis zum bestätigten
  Ende aller nativen Calls. Keine Exceptions oder fremdes Unwinding über DMM-Frames.
- Erforderliche Windows-Unwind-Metadaten.
- Native `union`, `pack(N)` und `align(N)`; Klassifikation berücksichtigt
  überlappende und unaligned Felder. Explizites Alignment wird bei konkretem
  V1-Bedarf vorgezogen, aktuell bleiben diese drei Fähigkeiten in Etappe 3.
- `_linux.dmm` und `_windows.dmm` werden beim Package-Laden anhand des
  Ausgabe-Targets ausgewählt, auch bei Cross-Compiling. Kein zusätzliches
  `target(...)`-Sprachfeature.
- Raw-Bindings für pthreads, glibc, Kernel32, UCRT und Winsock mit benötigten
  Konstanten, Fehlerzugriffen und nativen Datenlayouts.

Abnahme: Native Threads rufen DMM-Callbacks korrekt auf; alle für epoll, IOCP
und DNS erforderlichen Typen sind darstellbar.

## Etappe 4: Plattform-Runtime ohne C-Shim

- Threadstart, Join, manuelle Reset-Events und Prozessbeendigung nach DMM portieren.
  Linux verwendet pthreads; Windows `_beginthreadex` mit normaler Callback-Rückkehr
  und bestätigtem Join.
- Die privaten Thread-/Event-Symbole behalten ihren Vertrag und werden durch
  DMM-Exports implementiert.
- Der Compiler erzeugt die reguläre C-Startup-Funktion `main` direkt als
  ABI-Brücke zu `__dmm_runtime_main`.
- Expliziter Runtime-Komponenten-Build-Modus `--runtime-component`:
  Er emittiert Objektdateien ohne Application-Startup, Package-main-Wrapper oder
  automatische Runtime-Verknüpfung. Native Imports bleiben als Requirements
  verfügbar. Dieser Modus erlaubt ausschließlich den festgelegten privaten
  Export-Symbolraum für Runtime-Komponenten.
- Komponenten verwenden skalare/POD-Operationen, Pointer und native Calls;
  benötigte compilerseitige Basishelfer werden explizit als Abhängigkeit angegeben.
  Der Modus darf niemals seine eigene Plattform- oder Netzwerk-Runtime
  automatisch nachladen. So entsteht kein Bootstrap-Zyklus.
- Erst den Compiler bauen, dann DMM-Runtime-Objekte mit dem fertigen Compiler
  erzeugen und installieren. C-Startup und OS-Libraries werden beim finalen
  Application-Link eingebunden.
- Nach bestätigter Verhaltensgleichheit den Plattform-C-Shim aus Build und
  Installation entfernen.

Abnahme: Thread-, Wake-, Join- und Shutdown-Verträge sowie Plattform-Startup
funktionieren ohne eigenen Plattform-C-Code.

## Etappe 5: Netzwerk-Runtime vollständig nach DMM

- Gemeinsame Socket-/Operation-Besitzer, Fehlerübersetzung, Deadlines und Lifecycle.
- Linux-Reactor mit epoll/eventfd, dann Windows-Reactor mit IOCP und Overlapped-
  Operationen, anschließend DNS-Queue und Resolver-Worker.
- Die bestehende private Netzwerk-Schnittstelle bleibt während der Migration
  Integrationsgrenze; `stdlib/core/net` behält seine Source-API.
- Read-/Write-Slots, stabile Operation-Speicher, einmalige Veröffentlichung,
  Wake außerhalb von Locks, bestätigte Cancellation, keine vorzeitige
  Bufferfreigabe und begrenzte DNS-Admission bleiben erhalten.
- Lifecycle: Executor-Drain bei verfügbarem Netzwerk → DRAINING → Package-Cleanup
  → bestätigter Reactor-/Resolver-Abschluss.
- C- und DMM-Implementierung vorübergehend über eine Build-Option auswählbar;
  nach TCP/UDP-, IPv4/IPv6-, DNS-, Deadline-, Cancellation- und Race-Parität
  ausschließlich DMM installieren und Netzwerk-C-Shims entfernen.
- Endzustand: Native ABI, Ownership, Async und Atomics bleiben Compileraufgaben.
  Socket-, epoll-, IOCP- und DNS-Implementierungen leben in DMM-Libraries.

## Prüfung und Dokumentation

Jede Etappe aktualisiert Sprach-, IR-, Backend- und Runtime-Dokumentation nur
für tatsächlich abgeschlossene Fähigkeiten. Tests vergleichen C-Layouts,
kleine skalare Rückgaben, gemischte Parameter, Registererschöpfung, native
Structs und Hidden-Results. Spätere Prüfungen umfassen Callback-Threads,
Join/Context-Lifetime, Startup, Union/Packing und die vorhandenen Loopback-,
Race-, Cancellation- und Lifecycle-Szenarien als verbindliche Paritätskriterien.
