# Teststand der stdlib-Sprachlücken

Stand: 5. Oktober 2026. Die Implementierung läuft von oben nach unten weiter. Patterns/Heap-Moves und die definierten gespeicherten Borrow-Sprachmittel sind gezielt abgenommen; die vollständige Container-Loan-Freigabe bleibt offen. Siehe [Plan und offene Punkte](../../plans/stdlib-language-gaps.md).

## Automatisierte Abdeckung

[stdlib_language_gaps_test.cmake](../stdlib_language_gaps_test.cmake) führt den Vertrag stdlib_language_gaps_contract aus.

| Fixture | Umfang |
|---|---|
| language_gaps | Copy-Bounds, Callback-Inferenz, Owner-Patterns, take/replace und Box-Transfer |
| language_lifetimes | Verschachtelte Feld-/Payload-Provenienz, präzises replace, feste/relative/modulare Heap-Slots und Heap-Felder, List.pop/clear/truncate, Remove-Relocation, Ring-Wrap/Entnahme/Drain mit Größen-Guards, belegungsabhängige Slot-Drains und HashMap.clear mit borrowed Keys/Values, Heap-Feld-Kopien und Feld-Ersetzung, Moves und Enum-Verpackung, branch-lokale Guards, Referenzslots, Paket-Borrows, Setter und MutexGuard |
| language_collections | Owner-List/Deque/HashMap, Wachstum, Ring-Wrap, Kollisionen und Cleanup |
| language_files | File-Owner in Collections und fallible Propagation |
| language_closures | Shared/mut/once, benannte callable Interfaces, Captures und Cleanup |
| language_gaps_async | Verschachtelte und umgeordnete Future-Patterns, präzises replace, Borrow-Aggregat-Ergebnisse über block_on/.await()/futureComplete, typisiertes Polling, gepinnte Poller, Cancellation, benannte Callables, select/race/join, sleep und timeout |
| collection_allocation_faults | Isolierte Allocation-Fehler, unveränderte Inhalte, eingehende Owner, Retry und verbleibende Allocations |

Execution-Fixtures laufen bei O0/O1 auf dem Host. ELF-/COFF-Objekte und Intel-/ATT-Assembly werden zusätzlich ausgegeben; das ist keine Linux-Ausführung. Allocation-Fehler werden in einer isolierten stdlib-Kopie injiziert.

Negativfälle prüfen Consumption, Borrow-Escape, Alias-Konflikte, Lifetime-Zuordnungen, Slot-Zugriffe, Setter-Weiterleitung, Callable-Erasure und unzulässige Future-Operationen. Sie werden mit --emit=obj bei voreingestellter Optimierung auf Diagnosen geprüft und sind keine O0/O1-Execution-Tests.

## Tatsächlicher Prüfstand

Die vollständige Windows-Suite bestand nach Behebung der bisherigen Regressionen mit **57/57 Tests in 243,60 Sekunden**. Die anschließenden Container-, Ring-, Heap-Feld- und belegungsabhängigen Drain-Erweiterungen bestanden gezielt mit regression_rejection, async_semantic_unit, language_foundation_unit, resource_fault_unit und frontend_pipeline_unit (**6/6 insgesamt in 58,64 Sekunden**). **stdlib_language_gaps_contract bestand in 57,39 Sekunden**, bei O0/O1 einschließlich Objekt-/Assembly-Ausgaben und Negativfällen. Nach Ergänzung der positiven Feld-Ersetzung und des negativen Kapazitäts-Callback-Falls bestand der abschließende Vertragslauf ebenfalls (56,76 Sekunden). regression_positive und shared_language_contract bestanden vor den anschließenden Verfeinerungen. Die vollständige Windows-Suite wurde nach diesen anschließenden Änderungen nicht nochmals ausgeführt.

Gezielte Linux-O1-Ausführungen von language_lifetimes und language_gaps_async bestanden vor den belegungsabhängigen Drain-Erweiterungen; language_lifetimes wurde nach den Ring-/Heap-Feld-Erweiterungen erneut geprüft. Der neueste Linux-Lifetime-Lauf wurde nicht ausgeführt, weil die automatische Freigabeprüfung zweimal an einem Kapazitätsfehler ihres Prüfmodells scheiterte. Für die neuesten Änderungen liegt somit kein Linux-Ausführungsergebnis vor.

Unter Windows geprüft sind List.pop/clear/truncate, Remove-Relocation, modulare Ring-Koordinaten mit Größen-Guards, Deque.pushFront/popFront/clear, ganze Moves und Enum-Verpackungen, unabhängige Heap-Member-Loans und Kopien sowie belegungsabhängige Slot-Drains einschließlich HashMap.clear nach einem Insert mit borrowed Keys/Values. Ein Benutzer-Container prüft Kollisionen, Wrap und Feld-Ersetzung ohne stdlib-Namensregeln. Negativfälle prüfen lebende Ergebnisse und Kopien, Restwerte, stale Snapshots, unsigned Über-/Unterlauf, falsche Drain-Schritte, veränderte Belegung/Kapazität, geänderte Cursor, effektbehaftete oder veraltete Belegungs-Guards und Guard-Fakten, die einen Branch oder Match-Arm verlassen würden. Allocation-Relocation, beliebige dynamische Indexbeziehungen und präzise Hash-Key-Entnahme bleiben offen. Bei mehreren HashMap.insert-Aufrufen kann eine mögliche Allocation-Relocation den bisherigen Slot-Nachweis verlieren, sodass clear diese Loans weiterhin konservativ festhält.

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
