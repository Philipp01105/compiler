# Checked Auto-Deref für Place-Zugriffe und Methoden

Stand: 7. Oktober 2026. Status: umgesetzt und vollständig abgenommen.

## Verbindliche Sprachregel

Auto-Deref vereinfacht Zugriffssyntax, ohne Ownership, Mutation oder Pointer-Safety zu ändern.

- `p.x` und `p.method()` über checked Referenzen entsprechen den explizit dereferenzierten Formen.
- Für `values:&T[]` sind `values.length`, `values[i]` und `&values[i]` erlaubt. Indexierung bleibt ein Place mit unveränderten Bounds-Checks und Loan-Origins.
- Verschachtelte checked Referenzen verwenden die bestehende parenthesierte Syntax, etwa `&(&Point)` oder `&mut (&mut Point)`. Jede Shared-Schicht verhindert Mutation.
- Auto-Deref endet an Raw Pointern. `ptr:*Point` benötigt `(*ptr).x` beziehungsweise `(*ptr).method()`. Auch `&(*Point)` verbirgt keine Raw-Dereferenzierung.
- Compile-Time-Typmetadaten wie `.type` beschreiben weiterhin den Originaltyp. Bestehende direkte Raw-Pointer-Indexierung bleibt unverändert.
- Operatoren, Initialisierung, Assignment, Return und gewöhnliche Funktionsargumente bekommen kein Auto-Deref.
- Kein zusätzliches Auto-Borrow, keine neue `self`-Syntax, keine Deref-Traits, Smart-Pointer-Konversionen oder neue `unsafe`-Syntax. Vorhandene implizite Instanz-Receiver und Shared/Mut/Once-Verträge bleiben bestehen.

Der frühere Plan mit Auto-Deref für Raw Pointer wurde durch diese Regeln ersetzt. Die Ablehnung bisher akzeptierter impliziter Raw-Pointer-Member-/Methodenzugriffe ist eine beabsichtigte Sprachänderung.

## Umsetzung und Prüfpunkte

- [x] Wert-, checked- und Raw-Receiver mit expliziten Vergleichsfällen erfassen; generische Instanzen, Interface-Verträge, Overloads und callable Felder prüfen.
- [x] Member-/Index-Basen in Sema mit typisierten synthetischen `AST_EXPR_UNARY(TOKEN_STAR)`-Knoten normalisieren. Nur checked Schichten abtragen; vollständige Referent-Typen, Source-Spans und die vorhandene Analyse expliziter Dereferenzierung verwenden.
- [x] Erneute Analyse und AST-Clones bleiben idempotent: Bereits normalisierte Basen werden nicht nochmals dereferenziert. Generische Receiver mit mehreren konkreten Typen ausführen.
- [x] Raw-Pointer-Grenze vor Member-/Methodenauflösung prüfen; Diagnose nennt Raw-Receiver-Typ und explizite Schreibweise. Paket-/Typzugriffe und Typmetadaten bleiben unverändert.
- [x] Vorhandene Auflösungspriorität, Shared-/Mut-/Once-Verträge sowie Place-Pfade und Consumption-Regeln erhalten.
- [x] Borrow-Return- und Elisions-Origin-Suche durch explizite und eingefügte `*` führen. Dadurch bleiben checked Getter von owned Heap-Feldern nach Raw-Syntaxmigration gültig; lokale Borrow-Flucht wird weiterhin abgewiesen.
- [x] Typvergleich checked Array-Argumente berücksichtigt die vollständige AST-Form statt unterschiedlich kodierter Laufzeit-Pointer-Tiefen. `&i32[3]` und `&mut i32[3]` sind im Vertrag enthalten.
- [x] Bestehende IR-Senkung der expliziten Dereferenzierung verwenden. Receiver/Index genau einmal auswerten; Mutation am Original, keine unbeabsichtigten Moves/Kopien/Drops. O0/O1-Ausführung und ELF/COFF-/Assembly-Ausgaben prüfen.
- [x] Alte Raw-Pointer-Zugriffe in stdlib, Runtime, Beispielen, positiven Fixtures und generierten Testquellen auf explizite Dereferenzierung migrieren. Borrowed stdlib-Algorithmen verwenden jetzt checked Auto-Deref.
- [x] Sprachregel und Testdokumentation aktualisieren.
- [x] Gezielte Linux-Verträge und Netzwerk-Runtime-Test bestehen; vollständige Linux-Suite bleibt gemäß AGENTS.md CI.
- [x] Nach der letzten Codeänderung alle Windows-Targets bauen und die vollständige Windows-Suite als finalen Test bestehen.

## Vertragsabdeckung

`auto_deref_contract` führt die Fixture `tests/language/auto_deref/auto_deref.dmm` bei O0/O1 aus und prüft ELF-/COFF-Objekt- sowie Intel-/AT&T-Assembly-Ausgaben. Positive Fälle decken Member, Methoden, checked Arrays/Slices, nested Referenzen, mutable Receiver, generische Spezialisierungen, Overloads, callable Felder, Interface-Aufrufe, borrowed Ergebnisse, Async-Receiver, Seiteneffekte und Owner-Cleanup ab.

Negative Fälle sichern Shared-Schichten, konkurrierende Reborrows, Owner-/Element-Consumption, Once-Receiver, lokale Return-Flucht, fortbestehende Loans und Async-Receiver-Loans ab. Raw und gemischte Referenz-/Pointer-Receiver werden abgewiesen. Operatoren, Initialisierung, Assignment, Return und normale Argumente bleiben explizit; Auto-Borrow wird nicht eingeführt. Checked Array-/Slice-Bounds-Verletzungen müssen bei O0/O1 weiterhin zur Laufzeit abbrechen.

Die bisherigen konservativen Regeln für konkurrierende Reborrows über Slice-Views bleiben bestehen; explizite und automatische Dereferenzierung stimmen darin überein.

## Abnahme

Nach der letzten Codeänderung wurden alle Windows-Targets erfolgreich gebaut. Die abschließende vollständige Windows-Suite bestand mit **59/59 Tests in 150,10 Sekunden** (`ctest -j 4`), darin `auto_deref_contract` mit **5,72 Sekunden** und der Collections-/Closure-/Future-Vertrag mit 99,67 Sekunden.

Unter Linux bestanden nach dem Build der betroffenen Targets **4/4 gezielte Tests in 131,20 Sekunden**: Netzwerk-Runtime (0,14 Sekunden), stdlib-Komponenten (29,33 Sekunden), Auto-Deref-Vertrag (48,76 Sekunden) und Shared-Vertrag (52,92 Sekunden). Die vollständige Linux-Suite bleibt CI.

Alle Planpunkte sind erfüllt. Nach diesen finalen Läufen wurde ausschließlich die Ergebnisdokumentation aktualisiert.
