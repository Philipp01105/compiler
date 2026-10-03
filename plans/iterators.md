# Iteratoren über Source-Methoden

Die Datenstruktur implementiert `iter()`, `iterRef()` und optional `iterMut()` als parameterlose, nicht konsumierende Instanzmethoden. Jede Factory liefert einen konkreten Iterator-Struct mit `next() -> stdlib.Option<Item>`. Es gibt keine Index-Fallbacks, Collection-Sonderfälle, associated types oder Interface-Erasure.

```dmm
for (var x = list) { use(x); }
for (var &x = list) { inspect(x); }
for (var &mut x = list) { modify(x); }
```

`x` übernimmt im ersten Fall den tatsächlichen Item-Typ; ein benannter Source wird nicht konsumiert. Shared/Mut erfordern `&T` beziehungsweise `&mut T` als Item. Mutable Items und abgeleitete Borrows dürfen nur innerhalb ihres Durchlaufs leben. Produktions-Collections der stdlib erhalten in diesem Schritt keine Iteratoren; kleine Source-Fixtures prüfen das Sprachprotokoll.

## Umsetzung

- [x] Parser und AST unterscheiden klassische `for`-Schleifen und die drei Iterator-Bindings; klassische Schleifen bleiben unterstützt.
- [x] `self` als checked, adressierbaren Instanz-Receiver verfügbar machen: `self.field`, `&self`, `&mut self`; vorhandene Receiver-ABI verwenden, Schattenbindungen und Whole-Receiver-Ersetzung verbieten.
- [x] Whole-Receiver-Borrows mit impliziten und expliziten Feld-Places verbinden. Factory-Rückgaben behalten den Borrow-Origin und die Source-Lifetime.
- [x] Iterator-Schleifen vor Ownership-/Borrow-Prüfung in versteckte Bindings, `while` und einen `Option`-Match senken. Quelle und Factory genau einmal auswerten; temporäre owned Quellen hoisten.
- [x] Protokoll beim Method-Resolve strukturell prüfen: konkrete Iterator-Structs, parameterlose nicht konsumierende Instanzmethoden, echte stdlib-Option und passende Borrow-Modi.
- [x] Mutable Item-Loans mit der Iterations-Scope begrenzen. Abgeleitete und kopierte Loans behalten die Grenze, einschließlich Funktionsweitergabe, indirekter Stores, Aggregaten, Closures und Futures.
- [x] Bestehende Cleanup-Pfade für Fallthrough, leere Iteratoren, `continue`, `break`, verschachtelte Schleifen und frühe Rückgaben verwenden. Owned Items, borrowed owned Elemente und temporäre owned Sources prüfen.
- [x] Generische `TestList<T>` und unabhängigen Counter ohne Index-Zugriff beziehungsweise `iterMut` im DMM-Code implementieren. Der Standard-Item-Typ darf auch selbst ein Borrow sein.
- [x] Closure-Captures unterscheiden explizites `&` von `move` eines checked Borrows. Generische Methodenauflösung liest nur Funktions-Deklarationen als Funktionsparameter.
- [x] O0/O1-Ausführung, ELF/COFF-Objekte und Intel/AT&T-Assembly im `iterator_contract` prüfen. Editor-AST-Validierung und Serialisierung einschließen.
- [x] Spezifikation, Grammatik und Testdokumentation ergänzen.
- [x] Gezielte Linux-Verträge durchführen; volle Linux-Suite bleibt gemäß AGENTS.md CI.
- [x] Nach der letzten Codeänderung alle Windows-Targets bauen und die vollständige Windows-Suite als finalen Test bestehen.

## Abdeckung

[`tests/language/iterators/iterators.dmm`](../tests/language/iterators/iterators.dmm) enthält generische und unabhängige Source-Implementierungen, alle Bindings, Default-Reference-Items, manuelle Factory-Nutzung, checked Sources, generische Schleifen, Owned-Items, Destruktor-Zähler und Closure-/Future-Nutzung innerhalb eines Durchlaufs.

[`tests/iterator_test.cmake`](../tests/iterator_test.cmake) prüft zusätzlich fehlende oder falsche Protokolle, Shared-Mutation, Raw-Source-Zugriff, Borrow-Konflikte, Source-Moves, Borrow-Flucht und reservierte Receiver-Bindings.

## Validierung

Unter Linux bestehen `iterator_contract` (75,84 s) und `auto_deref_contract` (37,81 s). Die bestehenden Closure- und Collections-Fixtures wurden zusätzlich jeweils in O0/O1 erfolgreich kompiliert und ausgeführt. Der große `stdlib_language_gaps_contract` überschritt unter WSL sein bestehendes Zeitlimit von 180 s; deshalb erfolgte die gezielte Fixture-Prüfung. Die vollständige Linux-Suite bleibt CI.

Der unveränderte `stdlib_language_gaps_contract` wurde anschließend auf Benutzerwunsch unter WSL mit einem lokalen Zeitlimit von 900 s wiederholt: **1/1 bestanden in 709,05 s**. Das reguläre Testzeitlimit in CMake wurde nicht verändert.

Finale Windows-Abnahme: Alle Targets gebaut, danach **60/60 Tests bestanden in 157,78 s** (`ctest --test-dir build-language-gaps -j 4 --output-on-failure`). Der neue Iterator-Vertrag besteht mit O0/O1, ELF/COFF, beiden Assembly-Syntaxen, Editor-AST und 36 Negativfällen; der bestehende Language-Gap-Vertrag besteht ebenfalls (94,47 s). Danach wurde nur dieser Ergebnisnachweis ergänzt.
