# Teststand der stdlib-Sprachlücken

Stand: 5. Oktober 2026. Die Implementierung läuft von oben nach unten weiter. Patterns/Heap-Moves und die definierten gespeicherten Borrow-Sprachmittel sind gezielt abgenommen; die vollständige Container-Loan-Freigabe bleibt offen. Siehe [Plan und offene Punkte](../../plans/stdlib-language-gaps.md).

## Automatisierte Abdeckung

[stdlib_language_gaps_test.cmake](../stdlib_language_gaps_test.cmake) führt den Vertrag stdlib_language_gaps_contract aus.

| Fixture | Umfang |
|---|---|
| language_gaps | Copy-Bounds, Callback-Inferenz, Owner-Patterns, take/replace und Box-Transfer |
| language_lifetimes | Verschachtelte Feld-/Payload-Provenienz, umgeordnete Lifetime-Owner mit destroy, präzises replace auf Referenzen und Heap-Payloads, List.pop/clear, Referenzslots, Paket-Borrows, Setter, Binäradapter und MutexGuard |
| language_collections | Owner-List/Deque/HashMap, Wachstum, Ring-Wrap, Kollisionen und Cleanup |
| language_files | File-Owner in Collections und fallible Propagation |
| language_closures | Shared/mut/once, benannte callable Interfaces, Captures und Cleanup |
| language_gaps_async | Verschachtelte und umgeordnete Future-Patterns, präzises replace, Borrow-Aggregat-Ergebnisse über block_on/.await()/futureComplete, typisiertes Polling, gepinnte Poller, Cancellation, benannte Callables, select/race/join, sleep und timeout |
| collection_allocation_faults | Isolierte Allocation-Fehler, unveränderte Inhalte, eingehende Owner, Retry und verbleibende Allocations |

Execution-Fixtures laufen bei O0/O1 auf dem Host. ELF-/COFF-Objekte und Intel-/ATT-Assembly werden zusätzlich ausgegeben; das ist keine Linux-Ausführung. Allocation-Fehler werden in einer isolierten stdlib-Kopie injiziert.

Negativfälle prüfen Consumption, Borrow-Escape, Alias-Konflikte, Lifetime-Zuordnungen, Slot-Zugriffe, Setter-Weiterleitung, Callable-Erasure und unzulässige Future-Operationen. Sie werden mit --emit=obj bei voreingestellter Optimierung auf Diagnosen geprüft und sind keine O0/O1-Execution-Tests.

## Tatsächlicher Prüfstand

Die vollständige Windows-Suite bestand nach Behebung der bisherigen Regressionen mit **57/57 Tests in 243,60 Sekunden**. Die anschließenden Pattern-/Heap-/Future-Borrow-Änderungen wurden gezielt geprüft: **stdlib_language_gaps_contract bestand in 49,84 Sekunden**, bei O0/O1 einschließlich Objekt-/Assembly-Ausgaben und Negativfällen. Im selben Lauf bestanden regression_rejection, async_semantic_unit, language_foundation_unit, resource_fault_unit und frontend_pipeline_unit (6/6 insgesamt). regression_positive und shared_language_contract bestanden vor der anschließenden Verfeinerung der Future-Ergebnisfelder. Die vollständige Windows-Suite wurde nach diesen anschließenden Änderungen nicht nochmals ausgeführt.

Gezielte Linux-O1-Ausführungen des aktuellen language_lifetimes und language_gaps_async bestanden. Präzise Container-Loan-Freigabe ist bislang für List.pop/clear mit Kopien, Moves und Enum-Verpackung geprüft; weitere Remove-/truncate-/Ring-/Hash-Fälle bleiben offen.

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
