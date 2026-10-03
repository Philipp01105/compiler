# Teststand der stdlib-Sprachlücken

Stand: 7. Oktober 2026. Der [Folgeplan](../../plans/stdlib-language-gaps-next.md) ergänzt Ring-Wachstum, Rehash-Transfer, generische consuming Interface-Verträge, Wake-Kontextwechsel und Void-Komposition. Beliebige dynamische Index-/Key-Gleichheit und partielle Slot-Transfers bleiben konservativ. Siehe [Plan und offene Punkte](../../plans/stdlib-language-gaps.md).

## Automatisierte Abdeckung

[iterator_test.cmake](../iterator_test.cmake) prüft `for (var x = source)`, Shared-/Mut-Bindings und das durch Datenstrukturen implementierte `iter`/`iterRef`/`iterMut`-/`next`-Protokoll. Kleine generische und unabhängige DMM-Fixtures decken Ownership, Cleanup und Iterations-Lifetimes ab; Produktions-Collections werden hier noch nicht integriert. Siehe [Iterator-Plan](../../plans/iterators.md).

[auto_deref_test.cmake](../auto_deref_test.cmake) prüft checked Auto-Deref für Member, Methoden und Array-/Slice-Indexierung. Der Vertrag umfasst O0/O1-Ausführung, ELF/COFF und Assembly, Receiver-/Index-Auswertung genau einmal, Ownership/Cleanup, Borrow-Origins, Async-Receiver und Bounds-Traps. Negativfälle sichern Raw-Pointer-Grenzen, Shared-/Mut-/Once-Regeln und das Ausbleiben impliziter Wertkonvertierungen sowie zusätzlichen Auto-Borrows ab. Die borrowed stdlib-Algorithmen verwenden diese Zugriffssyntax. Status und finale Abnahme stehen im [Auto-Deref-Plan](../../plans/auto-deref-method-receivers.md).

Die abschließende Auto-Deref-Abnahme bestand unter Windows mit **59/59 Tests in 150,10 Sekunden** (`ctest -j 4`), darin der neue Vertrag mit 5,72 Sekunden. Unter Linux bestanden **4/4 betroffene Verträge in 131,20 Sekunden**, einschließlich Auto-Deref (48,76 Sekunden), Netzwerk-Runtime, stdlib-Komponenten und Shared-Vertrag. Nach diesen Läufen wurde nur die Ergebnisdokumentation aktualisiert; die vollständige Linux-Suite bleibt CI.

[stdlib_borrowed_algorithms_test.cmake](../stdlib_borrowed_algorithms_test.cmake) ergänzt Owner-Traversierung mit borrowed Predicates und mutable Aktionen. Die Fixture `language_algorithms_borrowed` prüft Short-Circuit, leere Eingaben, mutable/owned Captures, in-place Mutation und Destruktion. Named Array-Views werden über Initialisierung, Zuweisung, Funktionsargumente, Struct-Felder und Enum-Payloads geprüft; Gegenbeispiele sichern Alias-, Lifetime- und Consumption-Grenzen ab. Die abschließende vollständige Windows-Suite bestand mit **58/58 Tests in 546,59 Sekunden**; der neue Vertrag bestand darin mit 5,02 Sekunden. Unter Linux bestand der gezielte Vertrag einschließlich O0/O1, ELF/COFF, Assembly und Negativfällen mit **42,53 Sekunden**. Danach wurde nur die Dokumentation aktualisiert; die vollständige Linux-Suite bleibt CI.

[stdlib_language_gaps_test.cmake](../stdlib_language_gaps_test.cmake) führt den Vertrag stdlib_language_gaps_contract aus.

| Fixture | Umfang |
|---|---|
| language_gaps | Copy-Bounds, Callback-Inferenz, Owner-Patterns, take/replace und Box-Transfer |
| language_lifetimes | Verschachtelte Feld-/Payload-Provenienz, präzises replace, feste/relative/modulare Heap-Slots und Heap-Felder, List.pop/clear/truncate, Remove-Relocation, zusammenhängender Allocation-Transfer, Ring-Wrap/Entnahme/Drain mit Größen-Guards, belegungsabhängige Slot-Drains und HashMap.clear mit borrowed Keys/Values, Heap-Feld-Kopien und Feld-Ersetzung, Moves und Enum-Verpackung, branch-lokale Guards, Referenzslots, Paket-Borrows, Setter und MutexGuard |
| language_collections | Owner-List/Deque/HashMap, Wachstum, Ring-Wrap, Kollisionen und Cleanup |
| language_files | File-Owner in Collections und fallible Propagation |
| language_closures | Shared/mut/once, benannte callable Interfaces, Captures und Cleanup |
| language_gaps_async | Verschachtelte und umgeordnete Future-Patterns, präzises replace, Borrow-Aggregat-Ergebnisse über block_on/.await()/futureComplete, typisiertes Polling, gepinnte Poller, Cancellation, benannte Callables, select/race/join, sleep und timeout |
| collection_allocation_faults | Isolierte Allocation-Fehler, unveränderte Inhalte einschließlich borrowed List-Werten, eingehende Owner, Retry und verbleibende Allocations |

Execution-Fixtures laufen bei O0/O1 auf dem Host. ELF-/COFF-Objekte und Intel-/ATT-Assembly werden zusätzlich ausgegeben; das ist keine Linux-Ausführung. Allocation-Fehler werden in einer isolierten stdlib-Kopie injiziert.

Negativfälle prüfen Consumption, Borrow-Escape, Alias-Konflikte, Lifetime-Zuordnungen, Slot-Zugriffe, Setter-Weiterleitung, Callable-Erasure und unzulässige Future-Operationen. Sie werden mit --emit=obj bei voreingestellter Optimierung auf Diagnosen geprüft und sind keine O0/O1-Execution-Tests.

## Aktueller Folgeplan

Der Windows-Sprachlücken-Vertrag bestand mit 93,49 Sekunden, einschließlich O0/O1, Allocation-Fehler/Retry und ELF-/COFF-Objekt-/Assembly-Ausgaben. Neu abgedeckt sind Deque und Benutzer-Ring mit wiederholtem Wachstum und getrennten Entnahmen, tagged Benutzer-Rehash und HashMap mit mehreren kollidierenden Inserts sowie borrowed Keys/Values. Die instrumentierte Allocation-Fixture prüft jetzt List, Deque und HashMap mit geliehenen Daten und Retry.

Die zusätzliche [Ablehnungsmatrix](../stdlib_language_followup_test.cmake) prüft falsche Ring-/Rehash-Schritte, live Ergebnisse/Kopien, checked Slots, allgemeine once-Methoden, generische Aliase und Copy-Receiver, checked Receiver und must_consume-Erasure. `limit_hash_key_release` dokumentiert die weiterhin konservative Hash-Key-Entnahme. Bestehende Snapshot-/Callback-/Capture- und Borrow-Ergebnis-Fälle bleiben Bestandteil des Vertrags. Checked Snapshot-Weiterleitung und die letzten Receiver-Fälle bestanden ebenfalls im finalen Gesamtlauf.

Die **abschließende vollständige Windows-Suite bestand mit 57/57 Tests in 538,64 Sekunden**. Darin bestand der Sprachlücken-Vertrag erneut mit **90,96 Sekunden**. Nach diesem Lauf wurde kein Code mehr geändert; lediglich diese Ergebnisse wurden dokumentiert.

Der synchronisierte Executor-Test mit Wakes gegen Poll/Cancellation bestand zusätzlich 20 Wiederholungen. Unter Linux bestanden async_frontend_contract, async_runtime_contract und async_executor_contract (3/3, 17,84 Sekunden). language_lifetimes, language_closures und language_gaps_async bestanden bei O1, die instrumentierten Allocation-Fehler/Retry bei O0/O1 sowie der Executor-Unit-Test. Der umfangreiche Linux-Sprachlücken-Matrixlauf erreichte sein 180-Sekunden-Limit. Die vollständige Linux-Suite bleibt CI.

## Früherer Prüfstand vor dem Folgeplan

Die zusammenhängende Allocation-Transfer-Erweiterung vom 7. Oktober bestand mit **stdlib_language_gaps_contract in 78,23 Sekunden**. Sie prüft List-Wachstum, unabhängige pop-/clear-Freigabe, einmalige Destruktion borrowed Owner, checked Wrapper, Moves/Enum-Verpackung und wiederholte Aufrufe eines Benutzer-Owners. Die Allocation-Fehler-Fixture prüft borrowed List-Inhalte nach injiziertem Wachstumsfehler sowie Retry und anschließende Freigabe. Neue Negativfälle prüfen lebende Resultate/Kopien, checked Slots, Fehler-Returns und unvollständige oder veränderte Transfer-Schleifen. `regression_rejection`, `async_semantic_unit`, `language_foundation_unit`, `resource_fault_unit` und `frontend_pipeline_unit` bestanden mit **5/5 Tests in 16,24 Sekunden**. `regression_positive` und `shared_language_contract` bestanden ebenfalls mit **2/2 Tests in 54,48 Sekunden**. Für diese Erweiterung wurde keine Linux-Ausführung und keine vollständige Windows-Suite durchgeführt.

Die vollständige Windows-Suite bestand nach Behebung der bisherigen Regressionen mit **57/57 Tests in 243,60 Sekunden**. Die anschließenden Container-, Ring-, Heap-Feld- und belegungsabhängigen Drain-Erweiterungen bestanden gezielt mit regression_rejection, async_semantic_unit, language_foundation_unit, resource_fault_unit und frontend_pipeline_unit (**6/6 insgesamt in 58,64 Sekunden**). **stdlib_language_gaps_contract bestand in 57,39 Sekunden**, bei O0/O1 einschließlich Objekt-/Assembly-Ausgaben und Negativfällen. Nach Ergänzung der positiven Feld-Ersetzung und des negativen Kapazitäts-Callback-Falls bestand der abschließende Vertragslauf ebenfalls (56,76 Sekunden). regression_positive und shared_language_contract bestanden vor den anschließenden Verfeinerungen. Die vollständige Windows-Suite wurde nach diesen anschließenden Änderungen nicht nochmals ausgeführt.

Gezielte Linux-O1-Ausführungen von language_lifetimes und language_gaps_async bestanden vor den belegungsabhängigen Drain-Erweiterungen; language_lifetimes wurde nach den Ring-/Heap-Feld-Erweiterungen erneut geprüft. Der neueste Linux-Lifetime-Lauf wurde nicht ausgeführt, weil die automatische Freigabeprüfung zweimal an einem Kapazitätsfehler ihres Prüfmodells scheiterte. Für die neuesten Änderungen liegt somit kein Linux-Ausführungsergebnis vor.

Unter Windows geprüft sind List.pop/clear/truncate, Remove-Relocation, zusammenhängender Allocation-Transfer, modulare Ring-Koordinaten mit Größen-Guards, Deque.pushFront/popFront/clear, ganze Moves und Enum-Verpackungen, unabhängige Heap-Member-Loans und Kopien sowie belegungsabhängige Slot-Drains einschließlich HashMap.clear nach einem Insert mit borrowed Keys/Values. Ein Benutzer-Container prüft Kollisionen, Wrap und Feld-Ersetzung ohne stdlib-Namensregeln. Negativfälle prüfen lebende Ergebnisse und Kopien, Restwerte, stale Snapshots, unsigned Über-/Unterlauf, falsche Drain-Schritte, veränderte Belegung/Kapazität, geänderte Cursor, effektbehaftete oder veraltete Belegungs-Guards und Guard-Fakten, die einen Branch oder Match-Arm verlassen würden. Ring-/Hash-Allocation-Relocation, beliebige dynamische Indexbeziehungen und präzise Hash-Key-Entnahme bleiben offen. Bei mehreren HashMap.insert-Aufrufen kann eine mögliche Allocation-Relocation den bisherigen Slot-Nachweis verlieren, sodass clear diese Loans weiterhin konservativ festhält.

Keine vollständige Linux-Suite wurde zur abschließenden Abnahme ausgeführt. Unter Linux werden nur notwendige Tests ausgeführt; die vollständige Suite bleibt CI.

## Gezielte Prüfkommandos

Mit vorhandener Windows-Konfiguration und Compiler-Toolchain im PATH:

```powershell
cmake --build cmake-build-debug --target compiler --parallel 4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir cmake-build-debug -R stdlib_language_gaps_contract --output-on-failure
```

Für einen gezielten Async-Test mit vorhandener Linux-Konfiguration, im Repository-Verzeichnis:

```sh
cmake --build build-shared-linux-make --target compiler --parallel 4
build-shared-linux-make/compiler -O1 --emit=exe tests/stdlib/language_gaps_async/language_gaps_async.dmm -o build-shared-linux-make/language-gaps-async-focused
build-shared-linux-make/language-gaps-async-focused
```

Diese Kommandos prüfen die betroffenen Features; sie ersetzen keine vollständige CI-Abnahme.
