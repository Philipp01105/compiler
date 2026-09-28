# Teststand der stdlib-Sprachlücken

Stand: 5. Oktober 2026. Der Gesamtplan ist noch nicht abgeschlossen.
Der Implementierungsstand und die offenen Sprachmittel stehen in
[stdlib-language-gaps.md](../../plans/stdlib-language-gaps.md).

## Automatisierte Abdeckung

`stdlib_language_gaps_contract` wird durch
[stdlib_language_gaps_test.cmake](../stdlib_language_gaps_test.cmake) ausgeführt.

| Fixture | Geprüfter Umfang |
|---|---|
| `language_gaps` | Copy-Bounds, Callback-Inferenz, allgemeine Owner-Patterns, take/replace, Box-Transfer und Destruktion |
| `language_lifetimes` | Gespeicherte Borrows, direkte Feld-Lifetimes und Projektionen, konservative verschachtelte Origins, permanente Paket-Borrows, Binäradapter und Mutex-Guard |
| `language_collections` | Owner-List/Deque/HashMap, Wachstum, Ring-Wrap, Kollisionen, Entfernen, Ersetzen, clear/truncate und einmalige Destruktion |
| `language_files` | File-Owner in Collections und fallible Option-/Result-Propagation |
| `language_closures` | Explizite Captures, shared/mut/once, Owner-Captures, direkte Aufrufe, deferred Cleanup und Callback-Cleanup auf None-/Some-Pfaden |
| `language_gaps_async` | Poll/Poller-Grundlage, No-op-Waker/Context sowie Future-Payload-Loans bei Move, Match, Completion und Cancellation |
| `collection_allocation_faults` | Allocation-Fehler vor Relocation, unveränderte vorhandene Inhalte, Destruktion eingehender Owner, Retry und null verbleibende Allocations |

Die Execution-Fixtures laufen bei O0/O1 auf dem Host. Zusätzlich werden ELF und COFF
als native Objekte sowie Intel-/ATT-Assembly ausgegeben. Das ist keine Linux-Ausführung.
Allocation-Fehler werden nur in einer isolierten Kopie der installierten stdlib injiziert;
die produktive stdlib erhält keine Test-Intrinsics.

Die eingebetteten Negativfälle prüfen Copy/Send-Anforderungen, Use-after-move,
Consumption-Pflichten, Borrow-Escape, Alias-Konflikte, falsche Feld-/Payload-Lifetimes,
unzulässige Paket-Borrows, mutable Zugriffe durch shared Referenzen und Closure-Verträge.
Neu hinzugefügt ist die Ablehnung einer Rückgabe auf einen by-value skalaren Parameter.
Positive Gegenfälle übertragen Borrow-Payloads durch `branch` und `?`: Option, Result und ein
benutzerdefinierter Vertrag, shared/mutable Referenzen, Slices sowie geborgte Residuals,
jeweils mit Success-/Residual-Pfaden. Rekonstruierte Option-Rückgaben sind nun Execution-Tests.
Negativfälle prüfen lokale Flucht über Output und Residual, Konflikte nach Extraktion und
Weitergabe sowie falsche deklarierte Lifetimes. Bestehende Interface-/String-Rückgaben
bleiben durch die allgemeinen Frontend-, Execution- und Backend-Regressionen geschützt.
Die Option-Helfer werden mit trap-auslösenden, unbenutzten Owner-Callbacks für None und
mit ausgeführten Callbacks für Some geprüft. Destruktionszähler verlangen einmaliges
Cleanup. Closures mit importierter generischer Option-Rückgabe werden dabei normalisiert
und spezialisiert wie gewöhnliche Funktionen.
Diese Negativfälle werden mit `--emit=obj` bei der voreingestellten Optimierungsstufe
kompiliert und auf ihre Diagnose geprüft; sie sind keine O0/O1-Execution-Tests.

## Prüfkommandos

Im Repository-Verzeichnis mit der vorhandenen Windows-Konfiguration:

```powershell
$env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
cmake --build build-language-gaps --parallel 4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir build-language-gaps --output-on-failure --parallel 4
```

Für den gezielten Vertragstest:

```powershell
ctest --test-dir build-language-gaps -R stdlib_language_gaps_contract --output-on-failure
```

## Noch nicht abgenommen

Prüfstand vom 5. Oktober 2026: Der gezielte Sprachlücken-Vertrag mit der Propagation-Korrektur
ist bestanden. Die vollständige Windows-Suite wird nach dem erneuten Build geprüft.
Die abschließende Linux-Abnahme bleibt ausstehend.

Exklusives öffentliches Future-Polling, der gepinnte fromPoller-Adapter,
select2/race2/join2/timeout und nicht blockierende Timer sind noch nicht implementiert.
Für diese APIs gibt es keine erfolgreiche Abnahme. Ebenso offen bleiben vollständige
verschachtelte/interprozedurale Borrow-Provenienz, präzise Loan-Freigabe einzelner
Container-Slots, verschachtelte checked Referenzen/Referenzslot-Borrows und
Closure-Rückgaben über eine benannte Callable-Abstraktion.
Die abschließende Linux-Ausführung des aktuellen Sprachlücken-Stands steht aus.

Der frühere Abschluss der stdlib-Grundetappen ist ein historischer Prüfstand und
belegt diese neuen oder noch offenen Sprachmittel nicht.
