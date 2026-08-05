# Etappe 3: Runtime-enabling FFI

Implementiert sind native Function-Pointer-Typen, stabile `export "system"`-
Einstiegspunkte, eingehende und ausgehende ABI-Brücken mit gemeinsamem Klassifikator,
native Unions sowie `pack(N)` und `align(N)` für 1, 2, 4, 8 und 16.

Native und gewöhnliche DMM-Callable-Typen bleiben verschieden. Exports sind synchron,
nicht generisch und FFI-sicher. Kontexte werden durch Binding-Code bis zum bestätigten
Ende aller nativen Aufrufe gehalten; Exceptions und fremdes Unwinding sind ausgeschlossen.
Windows-Einstiegspunkte und synchrone Plattform-Funktionsbodies enthalten COFF-Unwind-
Metadaten, einschließlich großer Stackframes und veränderlichem Call-Stack.

Package-Dateien werden vor dem Parsen anhand des Ausgabe-Targets ausgewählt.
Manifest-Synchronisierung berücksichtigt beide Varianten. Raw-Bindings für glibc,
pthreads, Kernel32, UCRT und Winsock sind unter `stdlib/native` enthalten.

Die Callback-Prüfung umfasst Integer und gemischte Struct-Rückgaben, Hidden-Result-
Pointer, Registererschöpfung, indirekte Calls, unions, unaligned/packed Felder sowie
Alignment in lokalen Werten, DMM-Feldern, Arrays, Enum-Payloads und Globals.
Vier parallele native Threads rufen DMM-Callbacks auf; Ergebnisse und Kontexte werden
erst nach Join geprüft. Windows prüft zusätzlich OS-Unwinding durch verschachtelte
DMM-Frames und einen Callback mit großem Stackframe.

Die Binding-Prüfung vergleicht Typgrößen und Alignment mit den echten C-Headern und
führt pthread-Join/Mutex, epoll/eventfd, Win32-Events, IOCP, Winsock und DNS aus.
ELF- und COFF-Bindings werden jeweils auch vom anderen Host emittiert.
GCC und Clang werden als unabhängige Referenz-/Linktreiber geprüft.

Prüfstand: Windows-UCRT64 besteht den vollständigen CTest-Lauf mit 47/47 Tests.
Unter Linux bestehen 44/47 im ersten Gesamtlauf; `core_runtime_contract`,
`diagnostics_audit` und `diagnostics_cases` bestehen anschließend beim gezielten
erneuten Lauf (9/9). Im ersten Linux-Lauf traten eine Artefakt-Pfadprüfung und
Diagnostik-Timeouts auf; der erneute Lauf erfolgte ohne entsprechenden Code-Fix.
Damit wurden alle 47 Testfälle auf beiden Plattformen erfolgreich ausgeführt.
Nach den letzten Änderungen bestehen zusätzlich jeweils 11/11 gezielte Tests
für FFI, Bindings, Pipeline, Core-Runtime, Dumps und Manifest-Verarbeitung.

GCC und Clang bestehen Callback-Aufrufe bei `-O0` und `-O1` sowie manuell gelinkte
Intel- und AT&T-Assembly auf beiden Plattformen. Die Binding-Prüfungen bestehen
mit beiden Treibern, einschließlich Windows-Provider-Abfragen für AcceptEx und
ConnectEx. Compiler-Builds laufen mit strengen Warnungen; `git diff --check`
prüft die Änderungen.

Etappe 3 ist abgeschlossen. Etappen 4 und 5 sind weiterhin offen; eigene
C-Runtime-Shims bleiben installiert.
