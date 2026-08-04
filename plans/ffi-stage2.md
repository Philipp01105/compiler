# Etappe 2: implementierter Stand

Die allgemeine FFI umfasst 2A und 2B: skalare native Calls und Rückgaben sowie
native Structs by value auf x86-64 System V und Windows/MinGW-w64 UCRT64.
Etappe 3 erweitert diese Grundlage um Callbacks und weitere native Typformen;
die Plattform- und Netzwerk-Migration bleibt in Etappen 4 und 5.

## Implementierung

- Eigener C-ABI-Klassifikator, getrennt von der DMM-Aggregat-ABI. System V
  verwendet INTEGER-/SSE-Eightbytes, Stackübergabe, Register-Rollback bei
  erschöpften Registern und versteckte Ergebnis-Pointer. Windows verwendet
  positionsabhängige Register, Shadow Space, ausgerichtete temporäre Kopien
  und versteckte Ergebnis-Pointer.
- Integer- und `bit`-Rückgaben werden anhand ihrer tatsächlichen Breite
  normalisiert. Unbestimmte obere Registerbits gelangen nicht in DMM-Werte.
- `native-copy`-IR sichert Struct-Argumente während ihrer Auswertung. Eine
  spätere Argumentauswertung kann den bereits erfassten Wert nicht verändern.
- Native Feldzugriffe, Struct-Kopien und Arrays in nativen Structs verwenden
  bytegenaues C-Layout. Native Structs in gewöhnlichen DMM-Aggregaten und
  Arrays behalten ihre DMM-Slots. Der Code unterscheidet native Feld-Arrays
  von gewöhnlichen Arrays und Zeigern auf gewöhnliche Arrays.
- Verwendete Imports wählen das Plattform-Profil und bei `auto` den externen
  Linktreiber. `internal` nennt den nativen Import als konkreten Ablehnungsgrund.
- Library-IDs werden zu separaten `-lNAME`-Argumenten. `--native-library
  NAME=PATH` überschreibt eine ID mit einer Datei; `--native-library-dir DIR`
  ist wiederholbar. Symbol-Aliase und bereits im Frontend geprüfte Konflikte
  bleiben erhalten. Ungenutzte Imports erzeugen keine Library-Abhängigkeit.
- Der Treiber startet GCC/Clang ohne Shell, linkt isoliert und veröffentlicht
  erst das erfolgreiche Ergebnis. Fehlende Dateien und Linkfehler erhalten
  konkrete Diagnosen; die bisherige Ausgabedatei bleibt erhalten.
- Objekt- und Assembly-Ausgabe starten keinen Linkprozess.
  `dmm-native-link-v1` dokumentiert Target, Runtime-Profil, Suchpfade und
  tatsächlich verwendete Imports für manuelles Linking.

## Prüfung

Die C-Referenz prüft kleine signierte/unsigned Rückgaben, `_Bool`, gemischte
Integer-/Float-Argumente, Registererschöpfung, kleine und verschachtelte Structs,
INTEGER-/SSE-Rückgaben, Stackübergabe und versteckte Ergebnis-Pointer. Native
Werte innerhalb gewöhnlicher DMM-Aggregate und Arrays werden ebenfalls geprüft.
Ein Drei-Byte-Struct endet direkt an einer unzugänglichen Schutzseite; seine
Kopie darf kein viertes Byte lesen. Argumentreihenfolge und Snapshot-Verhalten
werden durch native Mutationen geprüft.

Die Linkprüfung umfasst echte Plattformfunktionen (`getpid` beziehungsweise
`GetCurrentProcessId`), logische Library-Suche, Pfade mit Leerzeichen, Overrides,
ungenutzte Imports, fehlende Libraries/Symbole, `internal`-Ablehnung sowie
ELF-/COFF-Objekt- und Assembly-Ausgabe ohne Linker. Fehlerfälle prüfen die
Prüfsumme der vorhandenen Ausgabedatei.

Windows: GCC 16.2.0 und Clang aus LLVM-MinGW 20260922, beide UCRT64; C-Referenzen
und DMM-Ausführung jeweils mit `-O0` und `-O1` erfolgreich. Die acht gezielten
Frontend-/ABI-/Linker-/Pipeline-/Dump-Verträge bestehen. In der Gesamtsuite
bestanden 44/45 Tests; der Standardtreiber-Test wurde durch einen expliziten
CTest-PATH zum konfigurierten GCC repariert und besteht in der Wiederholung.

Linux: GCC und Clang bestehen die C-Referenzen und DMM-Ausführung jeweils mit
`-O0` und `-O1`. Die acht gezielten Verträge einschließlich ABI und Linking bestehen.
Die Gesamtsuite hatte zunächst zwei Fehler im bisherigen Array-Verhalten
gefunden. Der C-Stride wurde fälschlich auch für Pointer auf gewöhnliche
Arrays verwendet; diese Regression ist korrigiert. Die beiden vollständigen
Wiederholungen bestehen: `regression_positive` und `direct_native_backend`.
Damit sind alle 45 registrierten Tests erfolgreich geprüft; auf Windows wurde
der einzelne PATH-Test, auf Linux die beiden Array-Regressionen nach ihren
Korrekturen wiederholt. Die Etappe-2-Abnahme ist abgeschlossen.
