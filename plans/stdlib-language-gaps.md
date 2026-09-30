# Sprachmittel für die stdlib

Dieses Dokument trennt allgemeine Sprachlücken von stdlib-Implementierung. Compiler-Sonderbehandlungen anhand von stdlib-Typ- oder Methodennamen sind kein Lösungsweg.

## Stand vom 5. Oktober 2026

Die Implementierungsarbeit läuft wieder in der Reihenfolge dieses Plans. **Patterns/Heap-Moves und die definierten gespeicherten Borrow-Sprachmittel sind umgesetzt und gezielt abgenommen.** Der Gesamtplan bleibt offen, bis auch die präzise Container-Loan-Freigabe vollständig umgesetzt ist. Die vorherigen Testregressionen sind behoben.

| Bereich | Implementierter Umfang | Verbleibende Grenzen |
|---|---|---|
| Copy und Callbacks | Allgemeine Copy-Bounds, Callback-Inferenz und konkrete Closure-Spezialisierung | Keine belegte Restlücke |
| Patterns und Heap-Moves | Owner-Patterns, take/replace, verschachtelte und umgeordnete Payload-Provenienz, getrennte alte/neue replace-Loans, destroy-Freigabe | Geschlossen im definierten Umfang; unbekannte Pfade bleiben konservativ |
| Gespeicherte Borrows | Verschachtelte Pfade, Referenzslots, mutable Paket-Borrows, synchrone interprozedurale Änderungen und Loan-Transfer aus Future-Ergebnissen | Geschlossen im definierten Umfang; Container-Freigabe wird separat verfolgt |
| Collections | Owner-List/Deque/HashMap, Allocation-Fehler-Regressionsfälle, List.pop/clear/truncate, bewiesene Remove-Relocation, Ring-Drains, getrennte Heap-Felder und vollständige belegungsabhängige Slot-Drains einschließlich HashMap.clear | Beliebige dynamische Indexbeziehungen, Allocation-Relocation und präzise Hash-Key-Entnahme bleiben offen |
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

Der Checker verfolgt relative Heap-Slots, Zähleränderungen und bewiesene Drain-Schleifen über Funktionszusammenfassungen. List.pop überträgt die Loans des letzten Slots; List.clear und truncate(0) geben gespeicherte Loans frei, auch über weiterleitende checked Wrapper. Kopierte oder entfernte Ergebnisse behalten ihre eigenen Loans, und Borrow-Zugriffe auf Container-Slots verhindern clear. Das gilt auch nach einem ganzen List-Move oder einer Enum-Verpackung.

Feste Slot-Indizes bleiben über unveränderte by-value Parameter und rohe Pointer-Parameter erhalten. Benannte Benutzer-Heap-Owner prüfen getrennte Referenten beim Entfernen eines festen Slots sowie take(storage[count-1]) ohne stdlib-Sonderregeln. Geänderte oder mutable geborgte Indexparameter erhalten keine Konstantenannahme. Ein Drain muss den passenden unsigned Zähler und exakt den nach dem Decrement bezeichneten Slot zerstören; verschobene Slots werden nicht als vollständige Freigabe akzeptiert. Schleifen verlieren nur die Slot-Präzision tatsächlich veränderter Container, nicht die aller gleichzeitig lebenden Container.

Unveränderte skalare Snapshots und einfache Feld-Getter bewahren relative Indizes wie `length()-1` über synchrone Aufrufe. Zuweisungen und mutable Aufrufe invalidieren diese Beziehungen. Frühe Return-Guards liefern exakte oder minimale unsigned Größenfakten. Solche Fakten bleiben innerhalb eines unbekannten Branches oder Match-Arms nutzbar und werden vor dessen Join verworfen. Dadurch geben `truncate(length()-k)` bei bewiesener Mindestgröße und konstante truncate-Grenzen bei passendem Größen-Guard die betroffenen Loans frei. Ein anschließendes clear gibt auch die behaltenen Präfix-Loans frei. Die Schleifengrenze muss eingefroren sein: `while(count>count-2)` erhält keine solche Annahme.

Eine strukturell bewiesene Relocation `for(i=index+1;i<count;i+=1) initialize(&storage[i-1],take(storage[i]))` verschiebt die Slot-Provenienz ohne stdlib-Namensregel. Geprüft sind List.remove des letzten und eines mittleren Elements, anschließendes pop sowie ein umbenannter Benutzer-Heap-Owner mit erase(length()-2). Die Operation muss direkt oder über einen unveränderten Parameter-Wrapper weitergeleitet werden. Entfernte Ergebnisse und verschobene Restwerte behalten ihre jeweiligen Loans. Allocation-Fehlerpfade bleiben konservativ, wenn die relativen Slot-Origins dadurch mehrdeutig sind.

Modulare Heap-Koordinaten erfassen Startposition, logischen Index und Kapazität in `(head+index)%slots`. Synchrone Pointer-Wrapper, ganze Moves und Enum-Payloads bewahren diese Beziehungen. Ein Ring-Drain muss denselben Startzeiger zerstören, ihn um exakt eine Position mit demselben Modulus weiterschieben und denselben unsigned Zähler decrementieren. Größenfakten müssen beweisen, dass die gehaltenen Slots im besuchten Präfix liegen; negative Offsets allein reichen wegen eines möglichen Zählerüberlaufs nicht aus. Geprüft sind Benutzer-Ringe über den Kapazitäts-Wrap, Tail-/Front-Entnahme, Leeren, Moves und Enum-Verpackung sowie Deque.pushFront/popFront/clear mit entsprechenden Guards. Deque.pushFront verwendet eine explizite modulare Rückwärtsbewegung. Veränderte Start-/Kapazitätsbeziehungen, ausgelassene Slots, lebende Entnahme-Ergebnisse und checked Slot-Borrows werden nicht vorzeitig freigegeben.

Heap-Member-Places wie `entries[index].value` erhalten die vollständige Feld-Provenienz. Entnahme und Destroy eines Felds geben die Loans benachbarter Felder nicht frei; ein Benutzer-Heap-Owner prüft unabhängige Referenten in solchen Slots.

Belegungsabhängige Heap-Slot-Drains sind jetzt allgemein nachweisbar: Unsigned Modulo-Indizes und unveränderte Probe-Schritte mit demselben Modulus belegen den Kapazitätsbereich; initialisierte Slots erhalten den konstanten Belegungswert. Eine vollständige Schleife von null bis zu dieser Kapazität mit Schritt eins gibt ihre Loans frei, wenn sie unter genau diesem Belegungs-Guard den ganzen Slot zerstört. Ganze Moves und Enum-Verpackungen bewahren den Nachweis. Änderungen an Belegung, Index oder Kapazität verwerfen die entsprechenden Fakten; ein Guard vor einem potenziell verändernden Callback genügt nicht. HashMap.insert prüft deshalb die Belegung vor dem Ersetzen erneut.

Geprüft sind HashMap.clear nach einem Insert, borrowed Keys und Values sowie ein umbenannter Benutzer-Container mit Kollisionen, Wrap, Feld-Ersetzung und Enum-Verpackung. Kopierte Heap-Member erhalten jetzt ebenfalls ihre eigenen Referenten-Loans und bleiben nach clear geschützt. Negativfälle prüfen falsche Guards, veränderte oder mutable geborgte Kapazität, Callback-Änderungen, veraltete Guards, geänderte Cursor, unbeschränkte Indizes und ausgelassene oder verschobene Destroy-Slots.

Die früheren falschen Konflikte in den Allocation-Fehler-Fixtures sind behoben. Beliebige dynamische Indexbeziehungen, Borrow-Provenienz beim Transport in neue Allocations und präzise Hash-Key-Entnahme bleiben offen. Mehrere HashMap.insert-Aufrufe bleiben insbesondere dann konservativ, wenn eine mögliche Allocation-Relocation die bisherigen Slot-Nachweise verliert; auch ein anschließendes clear muss deshalb nicht alle bisherigen Loans freigeben. Der Gesamtplan ist weiterhin nicht abgeschlossen.

## Prüfung

Die vollständige Windows-Suite bestand nach Behebung der bisherigen Regressionen mit 57/57 Tests (243,60 Sekunden). Die anschließenden Container-, Ring-, Heap-Feld- und belegungsabhängigen Drain-Erweiterungen bestanden gezielt mit regression_rejection, async_semantic_unit, language_foundation_unit, resource_fault_unit und frontend_pipeline_unit (6/6 insgesamt, 58,64 Sekunden). `stdlib_language_gaps_contract` prüfte O0/O1, ELF-/COFF-Ausgaben und die erweiterten Negativfälle (57,39 Sekunden). Nach Ergänzung der positiven Feld-Ersetzung und des negativen Kapazitäts-Callback-Falls bestand der abschließende Vertragslauf ebenfalls (56,76 Sekunden). regression_positive und shared_language_contract bestanden vor den anschließenden Verfeinerungen.

Gezielte Linux-O1-Ausführungen von language_lifetimes und language_gaps_async bestanden vor den belegungsabhängigen Drain-Erweiterungen. Der neue gezielte Linux-Lifetime-Lauf konnte nicht starten: Die automatische Freigabeprüfung scheiterte zweimal an einem Kapazitätsfehler ihres Prüfmodells, bevor der Befehl ausgeführt wurde. Für diese neuesten Änderungen liegt daher kein Linux-Ausführungsergebnis vor. Die vollständige Linux-Suite bleibt CI. Details und gezielte Kommandos stehen in [tests/stdlib/README.md](../tests/stdlib/README.md).
