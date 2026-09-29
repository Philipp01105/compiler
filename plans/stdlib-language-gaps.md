# Sprachmittel für die stdlib

Dieses Dokument trennt allgemeine Sprachlücken von stdlib-Implementierung. Compiler-Sonderbehandlungen anhand von stdlib-Typ- oder Methodennamen sind kein Lösungsweg.

## Stand vom 5. Oktober 2026

Die Implementierungsarbeit läuft wieder in der Reihenfolge dieses Plans. **Patterns/Heap-Moves und die definierten gespeicherten Borrow-Sprachmittel sind umgesetzt und gezielt abgenommen.** Der Gesamtplan bleibt offen, bis auch die präzise Container-Loan-Freigabe vollständig umgesetzt ist. Die vorherigen Testregressionen sind behoben.

| Bereich | Implementierter Umfang | Verbleibende Grenzen |
|---|---|---|
| Copy und Callbacks | Allgemeine Copy-Bounds, Callback-Inferenz und konkrete Closure-Spezialisierung | Keine belegte Restlücke |
| Patterns und Heap-Moves | Owner-Patterns, take/replace, verschachtelte und umgeordnete Payload-Provenienz, getrennte alte/neue replace-Loans, destroy-Freigabe | Geschlossen im definierten Umfang; unbekannte Pfade bleiben konservativ |
| Gespeicherte Borrows | Verschachtelte Pfade, Referenzslots, mutable Paket-Borrows, synchrone interprozedurale Änderungen und Loan-Transfer aus Future-Ergebnissen | Geschlossen im definierten Umfang; Container-Freigabe wird separat verfolgt |
| Collections | Owner-List/Deque/HashMap, Allocation-Fehler-Regressionsfälle und präzise List.pop/clear-Loans | Allgemeine Freigabe bei Remove/truncate, Ring-Wrap und Hash-Slots bleibt offen |
| Closures | Shared/mut/once, benannte callable Interfaces und Erhaltung von Consumption-Pflichten | Allgemeine consuming Interface-Methoden benötigen breitere Prüfung |
| Future-Komposition | Typisiertes Polling, fromPoller, select2/race2/join2, Timer und timeout | Weitere Scheduler-/Wake-Rennen; generische Wertkomposition verwendet für void einen Unit-Wert |

## Copy, Patterns und Ownership

Copy-Anforderungen und Callback-Spezialisierung folgen allgemeinen Typregeln. Value-Matches konsumieren move-only Sum-Enums unabhängig von Paket und Typname. Payload-Bindings übernehmen Ownership, Cleanup und Consumption-Pflichten; Borrow-Matches erhalten den Owner und erzeugen checked Payload-Borrows. Option-/Result-Propagation erhält Borrow-Origins einschließlich Residuals.

`take(place)` überträgt einen lebenden Wert ohne Destruktion. `replace(place,value)` wertet zuerst den Ersatz aus und gibt den alten Wert mit seinen bisherigen Loans zurück; der neue Slot hält die Loans des Ersatzes. Ganze tracked Werte einschließlich Referenzbindings erhalten Ownership-/Loan-Prüfungen; partielle tracked Moves bleiben eingeschränkt. Raw-Heap-Places behalten die expliziten Verantwortlichkeiten von initialize/destroy. Explizites destroy eines ganzen Werts gibt dessen gehaltene Loans frei; MUST_CONSUME darf damit nicht umgangen werden. Box-Transfer und Collections verwenden diese allgemeinen Operationen.

Regressionen prüfen verschachtelte und umgeordnete Future-Payloads über Funktionsrückgaben, take und consuming Matches sowie die unabhängige Freigabe ihrer Referenten. Lifetime-parametrisierte Owner-Payloads werden einmal zerstört. Generische Spezialisierungen übernehmen keine Lifetime-Namen aus einem fremden Funktionsscope; Callable-eigene Lifetime-Binder bleiben erhalten. Ein benutzerdefinierter Heap-Owner prüft replace auf gespeicherten Borrow-Payloads ohne stdlib-Namenssonderbehandlung.

## Gespeicherte Borrows

Verschachtelte Struct-Felder und Enum-Payload-Positionen erhalten vollständige Borrow-Pfade. Lifetime-Zuordnungen folgen auch temporären Argumenten, umgeordneten Payloads und projizierten Parametern. Synchrone Funktionszusammenfassungen übertragen Änderungen gespeicherter Referenzen einschließlich weitergeleiteter Setter; Branches und Schleifen vereinigen mögliche Origins konservativ. Lokale Flucht, falsche deklarierte Lifetimes und konkurrierende Zugriffe bleiben unzulässig. Temporäre Loan-Eltern bleiben bis zum Checker-Cleanup erhalten, damit Schleifen keine ungültigen Parent-Pointer verfolgen.

Checked Referenzen können Referenzen enthalten: `&(&i32)` und `&mut (&i32)`. Ein Borrow des Referenzslots schützt das Binding getrennt vom Referenten. Exklusive Slot-Zuweisung aktualisiert dessen Herkunft; ganze Referenzbindings können mit take/replace übertragen werden.

Immutable Paket-Borrows verlangen nachweislich permanentes Storage. Mutable Paket-Borrows sind ebenfalls implementiert: Holder bleiben fest, exklusive Zugriffe werden über synchrone Aufrufe geprüft und async Captures halten den Loan bis Completion/Cancellation. Binäradapter und MutexGuard behalten ihre checked Quellen; der Guard entsperrt beim Cleanup und ist nicht Send.

Liefert ein Future einen verschachtelten Borrow-Aggregatwert, bleiben die Captured Loans bis zur Übertragung an das Ergebnis erhalten. block_on, .await() und futureComplete dürfen diese Loans nicht vor der Ergebnismaterialisierung freigeben. Deklarierte Ergebnis-Lifetimes ordnen die Loans jedem Feld zu; reine Frame-Captures enden bei Completion. Positive Scope-Ende-/Projektionsfälle und negative Quellmutationen sowie vertauschte Ergebnis-Lifetimes prüfen benannte und temporäre Futures.

## Benannte Callables

Ein Interface mit genau einer `__invoke`-Methode kann als benannter Callable verwendet werden:

```dmm
interface Adder { func __invoke(value:i32)->i32; }
interface Accumulator { mut func __invoke(value:i32)->i32; }
interface OwnerFactory { once func __invoke()->Resource; }
```

Aufrufsyntax ist `callable(args)`. Shared/mut/once-Verträge gelten auch für erasure und konsumierende Closure-Umgebungen. Ein once-Aufruf konsumiert den owned Callable; nicht übertragene Captures werden einmal zerstört. `@[must_consume]` an einem Struct, Enum oder Interface bewahrt eine verpflichtende Consumption. Ein Callable mit verborgenem Future darf diese Pflicht nicht durch Konversion in ein gewöhnliches Interface verlieren.

## Future-Polling und Komposition

Die allgemeinen Compileroperationen sind:

- `futurePoll(&mut Future<T>, context:*void)->bit`: exklusives Polling ohne Handle-Consumption.
- `futureCancelPoll(&mut Future<T>, context:*void)->bit`: bestätigt Cancellation, ohne den Handle zu konsumieren.
- `futureComplete(Future<T>)->T`: konsumiert einen bereits fertigen Handle.
- `futureCancelComplete(Future<T>)->void`: konsumiert einen bestätigt gecancelten Handle.
- `asyncContext()->*void`: aktueller Kontext innerhalb einer async Funktion.

Terminalzustände werden im Frame geprüft. Raw-Kontexte und native Frames bleiben eine FFI-Vertrauensgrenze. Bereits fertige Handles werden durch await/block_on ohne erneuten Poll konsumiert.

`asynchronous.poll` liefert `FuturePoll<T>.Pending(Future<T>)` oder `Ready(T)`; `pollVoid` verwendet den separaten VoidFuturePoll. Pending überträgt Handle und Loans an den Aufrufer. `fromPoller<T,P>` besitzt den Poller in einem gepinnten async Frame und parkt bei Pending. Nach begonnenem Funktionskörper bestätigt deferred Cancellation den Abbruch vor Cleanup. Cancellation vor dem ersten Body-Poll führt nur Parameter-Cleanup aus; native Anfangszustände müssen deshalb schon sicher zerstörbar sein.

`select2` pollt beide Eingaben und liefert das Ergebnis sowie den weiterhin verpflichtend zu konsumierenden anderen Handle. Dieser kann bereits fertig sein. `race2` bestätigt Cancellation des Verlierers vor der Rückgabe. `join2` liefert `Joined<T,U>` mit left/right. Void-Komposition verwendet einen Unit-Payload oder das separate Void-Polling.

`asynchronous.timers` stellt Timer, sleep und timeout bereit. Ein Worker schläft in Abschnitten von höchstens einer Millisekunde, prüft Cancellation und weckt einen retained Kontext. `timeout` liefert `Timed<T>.Completed(T)` oder `Elapsed` nach bestätigter Cancellation. Ein gestarteter Timer darf erst nach Completion-/Cancellation-Bestätigung zerstört werden; andernfalls trappt der Destruktor. Die Adapter erledigen diesen Ablauf.

## Offene Container-Loan-Freigabe

Der Checker verfolgt relative Heap-Slots, Zähleränderungen und bewiesene Drain-Schleifen über Funktionszusammenfassungen. List.pop überträgt die Loans des letzten Slots; List.clear gibt gespeicherte Loans frei. Kopierte oder entfernte Ergebnisse behalten ihre eigenen Loans, und Borrow-Zugriffe auf Container-Slots verhindern clear. Das gilt auch nach einem ganzen List-Move oder einer Enum-Verpackung. Die früheren falschen Konflikte in den Allocation-Fehler-Fixtures sind behoben. Remove/truncate, Ring-Wrap und Hash-Slot-Zuordnung benötigen weiterhin eine konsistente Lösung und gezielte Regressionen.

## Prüfung

Die vollständige Windows-Suite bestand nach Behebung der bisherigen Regressionen mit 57/57 Tests (243,60 Sekunden). Danach bestand der um die neuen Pattern-/Heap-/Future-Borrow-Fälle erweiterte `stdlib_language_gaps_contract` bei O0/O1 (49,84 Sekunden), einschließlich ELF-/COFF-Ausgaben und Negativfällen. Im selben Lauf bestanden regression_rejection, async_semantic_unit, language_foundation_unit, resource_fault_unit und frontend_pipeline_unit (6/6 insgesamt). regression_positive und shared_language_contract bestanden vor der anschließenden Verfeinerung der Future-Ergebnisfelder.

Gezielte Linux-O1-Ausführungen des aktuellen language_lifetimes und language_gaps_async bestanden. Die vollständige Linux-Suite bleibt CI. Details und gezielte Kommandos stehen in [tests/stdlib/README.md](../tests/stdlib/README.md).
