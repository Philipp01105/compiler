# Etappe 4: Plattform-Runtime in DMM

Threadstart, Join, manuelle Reset-Events und Prozessbeendigung sind in
`src/runtime/platform/platform_linux.dmm` und `platform_windows.dmm` implementiert.
Die privaten Thread-/Event-/Exit-Symbole und ihre Ownership-Verträge bleiben erhalten.
Linux verwendet pthreads mit korrekt initialisiertem libc-TLS, Mutex-Predicate und
Condition-Variable-Schleife. Windows verwendet `_beginthreadex`, normale Rückkehr
des DMM-Callbacks, bestätigtes Join und `CloseHandle`. Ressourcen/API-Fehler bleiben
fatal. Context-Speicher wird erst nach bestätigtem Thread-Ende freigegeben.

Der Compiler erzeugt `main` als native ABI-Brücke zu `__dmm_runtime_main`.
Reguläres Plattform-/CRT-Startup initialisiert den Prozess; die vorhandene Bridge
führt Package-Init, DMM-main, Executor-Drain und Cleanup aus und erhält den Exitcode.
Fataler Exit aus einem Worker beendet weiterhin den gesamten Prozess.

`--runtime-component` emittiert standardmäßig ein Objekt, optional Assembly.
Der Modus verlangt ein Library-Package und erlaubt nur die festgelegten privaten
Plattform-Exports plus den Thread-Einstieg. Gewöhnliche und importierte DMM-Helfer
bleiben lokal, damit Application-Objekte dieselben Packages verwenden können.
Native Imports werden explizit im `dmm-native-link-v2`-Inventar mit dem Profil
`component` aufgeführt. Application-Startup, automatische Runtime-Verknüpfung,
implizite Basishelfer, erreichbare Async-/Drop-Bodies und dynamische globale
Initialisierung/Cleanup sind ausgeschlossen. Explizit deklarierte native
Basishelfer können als externe Abhängigkeit angegeben werden. Windows-Stackprobes
bleiben eine Abhängigkeit des regulären Compiler-Treibers.

CMake baut zuerst den Compiler, dann mit ihm das Plattformobjekt. Änderungen am
Compiler, den Komponenten und Raw-Bindings erzeugen es neu; ein Bootstrap-Zyklus
entsteht nicht. Die Komponente benötigt selbst ausschließlich OS-/CRT-Imports.
Der bestehende Artefaktname `platform-shim.o` bleibt zur Kompatibilität erhalten,
bezeichnet jetzt ausschließlich DMM-Code. Installation enthält dieses Objekt und
sein Linkinventar. Der Default-Build oder die Targets `dmm_platform_runtime` und
`dmm_network_runtime` müssen vor der Installation gebaut sein.

`network-shim.a` enthält übergangsweise das verbleibende Netzwerk-C-Objekt und das
DMM-Plattformobjekt. GNU- und LLVM-Archivwerkzeuge können dieses Bundle erzeugen;
auch Windows-Clang benötigt dafür keinen relocatable COFF-Linker. Die Linkschicht
prüft das Target jedes Objektmembers. Nur das benötigte Bundle wird verknüpft.
`platform_shim.c`, `platform_bundle.c`, `platform_entry.c` und `network_bundle.c`
sind aus Build und Repository entfernt. `platform_shim.h` bleibt als private
C-ABI-Deklaration für die Netzwerkimplementierung und unabhängige Vertragsprüfungen.

Prüfung: Der neue `runtime_component_contract` prüft ELF/COFF, `-O0`/`-O1`, beide
Assembly-Syntaxen, erlaubte Exports, fehlende rekursive Runtime-Abhängigkeiten und
Bootstrap-Fehlerdiagnosen. Native C-Vertragsprüfungen bestätigen TLS/errno, zwei
parallele Threads, bestätigtes Join, Signal vor Wait, wiederholtes Wait ohne Reset
und Broadcast-Wake. Bestehende Startup-, Exit-, Executor-, Shutdown- und Netzwerk-
Lifecycle-Prüfungen verwenden die DMM-Komponente.

Windows-UCRT64: vollständiger CTest-Lauf 48/48 bestanden. GCC und Clang bestehen
zusätzlich Callback-/Struct-ABI und manuell gelinkte Intel-/AT&T-Assembly.
Ein frischer Windows-Clang-Build mit strengen Warnungen erzeugt Compiler,
DMM-Plattformobjekt und LLVM-Archiv und besteht Bootstrap- und Loopback-Prüfung.
Eine mit LLVM-ar erzeugte Netzwerkbibliothek läuft über Windows-Clang; ein
installierter Compiler startet ein Netzwerkprogramm mit seinen installierten
Runtime-Artefakten. Dieselbe Installationsprüfung besteht unter Linux/Clang.
Linux: vollständiger CTest-Lauf ebenfalls 48/48 bestanden, ohne Wiederholung.
Die abschließenden Bootstrap-Abhängigkeits- und Archivprüfungen bestehen zusammen
mit den betroffenen Plattform-, Netzwerk- und FFI-Verträgen auf beiden Systemen
(jeweils 10/10). GNU-Make-Konsumenten sind ausdrücklich hinter dem Komponenten-
Target angeordnet, damit parallele Builds das gemeinsame Objekt nur einmal erzeugen.
Ein erzwungener paralleler Neubau bestätigt diese Reihenfolge.

Etappe 5 bleibt offen: Die Netzwerkimplementierung selbst ist weiterhin in C.
