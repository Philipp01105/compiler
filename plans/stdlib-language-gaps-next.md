# Nächste Arbeiten: Collections, Closures und Future-Komposition

Stand: 7. Oktober 2026. Grundlage ist [stdlib-language-gaps.md](stdlib-language-gaps.md). Die folgenden Abschnitte halten den Arbeitsauftrag fest; der Abnahmestand darunter trennt implementierte Regeln von verbleibenden konservativen Grenzen.

## Abnahmestand der Umsetzung

- [x] 1a: Wiederholter zusammenhängender Transfer, getrennte Owner, checked Wrapper, Moves/Enum-Payloads, Borrow-Owner-Cleanup sowie Allocation-Fehler/Retry sind durch positive und negative Fixtures abgedeckt.
- [x] 1b: Ganzer Ring-Präfix wird nach linearem Scratch-Storage transportiert; Pointer-/Kapazitäts-/Head-Commit erhält die logischen Slot-Loans. Benutzer-Ring und Deque prüfen Wrap, wiederholtes Wachstum und getrennte Entnahmen/Freigabe. Allocation-Fehler behalten Inhalte; Retry, Cleanup und Loan-Freigabe sind instrumentiert geprüft.
- [x] 1c: Vollständiger tagged Scan mit modulo-begrenztem Probing transportiert ganze Einträge einschließlich getrennter Schlüssel-/Wert-Origins. Benutzer-Tabelle und HashMap prüfen Kollisionen, Wrap, mehrere Inserts und clear. Borrowed Keys/Values und eingehende Owner werden auch bei injizierten Fehlern geprüft.
- [x] 2: Feste und affine Snapshot-Beziehungen sowie checked Weiterleitung sind abgesichert. Allgemeine Schlüsselgleichheit durch beliebige Callbacks ist nicht bewiesen; `limit_hash_key_release` hält diese Grenze ausdrücklich als reproduzierbaren Ablehnungsfall fest. Es gibt keine pauschale Loan-Freigabe nach remove.
- [x] 3: Allgemeine once-Methoden, dynamischer Dispatch und generische Bounds bewahren Consumption einschließlich lokaler Aliase und Copy-Receiver. Der Backend-Cleanup erfolgt am konsumierenden Aufruf. Shared/mut/once-Borrows und must_consume-Erasure werden mit Gegenbeispielen geprüft. Verschachtelte konsumierende Argumente werden nur einmal geprüft.
- [x] 4: fromPoller und select2 lesen den Kontext bei jedem Poll neu, auch während verzögerter Cancellation. Tests prüfen Kontextwechsel, externe Cancellation der Kompositionen, Cancellation vor dem ersten Poll und einmaligen Cleanup. Synchronisierte gleichzeitige Wakes gegen Cancellation ergänzen die vorhandenen Executor-Races. asUnit/selectVoid/raceVoid/joinVoid/timeoutVoid ergänzen direkte Void-Adapter mit den bestehenden Ownership-Regeln.
- [x] 5: Abschließende vollständige Windows-Abnahme bestanden: **57/57 Tests in 538,64 Sekunden**, darin stdlib_language_gaps_contract mit **90,96 Sekunden** einschließlich O0/O1, Allocation-Fehler/Retry, ELF-/COFF-Objekten, Assembly und Ablehnungsmatrix. Der synchronisierte Executor-Test bestand zusätzlich 20 Wiederholungen. Linux: language_lifetimes, language_closures und language_gaps_async bei O1, Allocation-Fehler/Retry bei O0/O1, Executor-Unit sowie drei Async-Verträge bestanden. Der vollständige Linux-Sprachlücken-Matrixlauf erreichte sein 180-Sekunden-Limit; die vollständige Linux-Suite bleibt CI. Spec, Hauptplan und Fixture-/Async-Dokumentation sind aktualisiert.

Die positiven Ausführungsfälle stehen in `tests/stdlib/language_lifetimes`, `language_closures`, `language_gaps_async` und `collection_allocation_faults`. `tests/stdlib_language_followup_test.cmake` enthält die zusätzliche Ablehnungsmatrix. Die schon vorhandenen Snapshot-, Callback-, Capture- und Borrow-Ergebnis-Tests bleiben Bestandteil von `stdlib_language_gaps_contract`.

Weiterhin konservativ bleiben beliebige dynamische Index-/Key-Gleichheit, partielle statt ganzer Slot-Transfers und abweichende Transfer-/Commit-Schleifen. Größenfakten müssen nach verändernden Aufrufen erneut beobachtet werden. Diese Grenzen sind Bestandteil der Abnahme von Abschnitt 2, keine behaupteten Sprachfähigkeiten.

## Vorgehen

- Vorhandene Änderungen erhalten und den zuletzt bestandenen Prüfstand als Ausgangspunkt verwenden.
- Je Teilproblem zuerst einen konkreten Reproduktionsfall ergänzen, dann Compiler-/Runtime-Regeln oder stdlib korrigieren und gezielt prüfen.
- Compilerregeln anhand von Places, Kontrollfluss, Typen und Ownership beweisen. Keine Sonderbehandlung von stdlib-Typ- oder Methodennamen.
- Positive Freigabe-Fälle stets mit Negativfällen für lebende Ergebnisse, Restwerte und Alias-Konflikte absichern.
- Jeden abgeschlossenen Schritt im Hauptplan und im Teststand dokumentieren. Unbewiesene Fälle bleiben konservativ und ausdrücklich offen.

## 1. Collections: Allocation-Transfer vervollständigen

### 1a. Zusammenhängenden Transfer weiter absichern

Die vorhandene Erweiterung auf wiederholtes Wachstum, mehrere Owner gleichzeitig, borrowed Keys/Values beziehungsweise mehrere Borrow-Felder und checked Weiterleitung prüfen. Fehler-/Retry-Pfade müssen Inhalt, Ownership und Loan-Provenienz erhalten. Insbesondere prüfen, dass positive Freigaben aus einem nachgewiesenen Transfer folgen und keine Loans durch Pointer-Austausch verloren gehen.

### 1b. Ring-Wachstum

Den Nachweis von `take(old[(head+i)%slots])` nach `initialize(&fresh[i],...)` für einen bewiesenen belegten Präfix ergänzen. Die logischen Slot-Koordinaten müssen nach dem Wechsel auf die neue Allocation und `head=0` erhalten bleiben. Start-/Kapazitätsänderungen erst nach dem belegten Transport übernehmen.

Abnahme: Deque und umbenannter Benutzer-Ring mit Wrap, pushFront/pushBack, wiederholtem Wachstum, popFront/popBack und clear; getrennte Freigabe der Referenten; einmaliger Owner-Cleanup; Allocation-Fehler und Retry. Negativfälle: ausgelassene Slots, falscher Modulus/Start, veränderte Grenzen, lebende Entnahmen und checked Slot-Borrows.

### 1c. Belegungsabhängiges Rehashing

Einen vollständigen Scan der alten Allocation unter einem gültigen Belegungs-Guard und den Transport ganzer Einträge in die neue Allocation nachweisen. Probing muss innerhalb der neuen Kapazität bleiben. Schlüssel- und Wert-Origins getrennt erhalten; Callback-Effekte dürfen Belegungs- oder Kapazitätsfakten nicht unbemerkt invalidieren.

Abnahme: mehrere HashMap.insert-Aufrufe, Kollisionen, Probe-Wrap, Ersetzung, Rehashing, anschließend clear; borrowed Keys und Values; Benutzer-Container ohne Namenssonderregeln. Fehlerpfade behalten die alten Einträge und konsumieren eingehende Owner korrekt. Lebende Kopien und checked Eintrags-Borrows verhindern vorzeitige Freigabe.

## 2. Collections: präzisere Entnahmen und dynamische Indizes

Zunächst unveränderte Index-Snapshots und nachgewiesene Beziehungen über checked Wrapper erweitern. Danach prüfen, welche Beziehung zwischen gespeichertem Schlüssel, Suchschlüssel und gefundenem Slot bei HashMap.remove/removeBorrowed allgemein beweisbar ist.

Abnahme: Entfernung gibt ausschließlich die Loans des tatsächlich entfernten Werts beziehungsweise zerstörten Keys frei; Restwerte bleiben geschützt. Tombstones, Kollisionen, nicht gefundene Keys, Callback-Änderungen und lebende Rückgabewerte abdecken.

Beliebige Laufzeitindizes und undurchsichtige Hash-/Gleichheits-Callbacks erhalten keine erfundene Gleichheitsannahme. Wo die Beziehung nicht beweisbar ist, konkrete Grenze und Reproduktionsfall dokumentieren.

## 3. Closures und konsumierende Interfaces

Allgemeine `once`-Interface-Methoden mit anderen Namen als `__invoke` untersuchen: strukturelle Implementierung, generische Bounds, Interface-Konversion, dynamischer Dispatch, Ownership-Checker und Backend-Cleanup müssen denselben Receiver-Vertrag erhalten.

Prüfmatrix:

- Shared-/mut-/once-Kompatibilität zwischen Interface und konkreter Methode.
- Owned Aufruf konsumiert genau einmal; zweiter Aufruf und Aufruf über checked Borrow werden passend abgewiesen.
- Mehrere Methoden im Interface und generische Implementierungen bewahren den Vertrag.
- Move-Captures, borrowed Captures und nicht zurückgegebene Ressourcen werden korrekt übertragen oder einmal zerstört.
- Versteckte Futures und `@[must_consume]` bleiben bei Konversionen, Wrappern und Rückgaben verpflichtend zu konsumieren.

Nachgewiesene Fehler allgemein beheben; bestehende Closure-Spezialisierung und Callback-Inferenz dabei absichern.

## 4. Future-Komposition und Wake-/Cancellation-Rennen

Vorhandene Executor-Race-Tests als Basis verwenden und gezielte Fälle für `fromPoller`, select2, race2, join2 und timeout ergänzen. Synchronisierte Testabläufe bevorzugen; Sleeps allein sind kein zuverlässiger Race-Nachweis.

Prüfmatrix:

- Wake während Poll, zwischen Pending und Park sowie mehrere gleichzeitige Wakes: keine verlorene Benachrichtigung und kein gleichzeitiges Polling desselben Frames.
- Bereits fertige und gleichzeitig fertige Kinder; select2 liefert einen weiterhin korrekt konsumierbaren Resthandle.
- Cancellation vor dem ersten Poll, während Poll/Park und während verzögerter Bestätigung.
- Cancellation des äußeren Kompositions-Futures beendet beide Kinder und deren Cleanup vollständig.
- Ready gegen Timeout/Cancellation: Ergebnis, Frame, Waker und native I/O-State werden genau einmal übernommen oder zerstört.
- Retained Waker nach Completion/Cancellation bleibt sicher; keine Wiederbelebung abgeschlossener Tasks.
- Borrowed Aggregate-Ergebnisse übernehmen ihre Loans vor Freigabe der Captures; abgebrochene Kinder geben ihre Loans erst nach Bestätigung frei.

Anschließend den tatsächlichen Bedarf für direkte Future<void>-Komposition prüfen. Zunächst die bestehende Unit-/Void-Polling-Lösung und gemischte Komposition absichern. Falls eine direkte void-API sinnvoll und mit den Typ-/Ownership-Regeln vereinbar ist, passende Adapter ergänzen; keine pauschale Lockerung der generischen Payload-Regeln.

## 5. Validierung und Abschluss

- Pro Compileränderung relevante Sema-/Ownership-/Frontend-Tests sowie gezielte positive und negative Regressionen.
- `stdlib_language_gaps_contract`: O0/O1-Ausführung, Allocation-Fehler und Objekt-/Assembly-Ausgaben.
- Bei Runtimeänderungen Executor-Unit- und Async-Runtime-/Executor-Verträge; synchronisierte Race-Fälle wiederholt ausführen.
- Gezielte Linux-Ausführung für betroffene Collections-/Closure-/Async-Fälle, sofern Toolchain verfügbar. Die vollständige Linux-Suite bleibt gemäß AGENTS.md CI.
- Nach der zusammenhängenden Änderung die vollständige Windows-Suite ausführen.
- Hauptplan, LANGUAGE_SPEC und Fixture-/Async-Dokumentation an den tatsächlich bestandenen Umfang anpassen. Offene Punkte und nicht ausgeführte Prüfungen ausdrücklich nennen.

Die Implementierung folgt dieser Reihenfolge. Sie arbeitet möglichst viele belegte Lücken ab; jeder Bereich wird erst nach bestandener Abnahme als erledigt markiert.
