# Option/Result-Fehlerpropagation mit `?`

## Zusammenfassung

Postfix-`?` wird als allgemeiner, interface-basierter Propagationsoperator eingeführt. Der Operand wird genau einmal
by-value ausgewertet. Bei Erfolg liefert `?` den `Output`; bei Fehler wird der `Residual` über `FromResidual` in den
Funktionsrückgabetyp umgewandelt und über den normalen Return-Pfad inklusive `defer`- und Drop-Cleanup zurückgegeben.

## Öffentlicher Sprach- und Stdlib-Contract

```dmm
pub enum Propagation<Output,Residual> {
    pub Continue(Output),
    pub Break(Residual),
}

pub enum NoneResidual { pub None, }

pub interface Propagate<Output,Residual> {
    pub static func branch(value:Self)
        -> Propagation<Output,Residual>;
}

pub interface FromResidual<Residual> {
    pub static func fromResidual(residual:Residual) -> Self;
}
```

- Interfaces erhalten generische Parameter und statische Anforderungen. Strukturen und Enums erfüllen sie weiterhin
  strukturell.
- Enums erhalten statische Methoden. Bei Enums mit Methoden trennt `;` Varianten und Methodenteil.
- Interface-Bounds akzeptieren Spezialisierungen wie `T:Propagate<O,R>`.
- `Option<T>` implementiert `Propagate<T,NoneResidual>` und `FromResidual<NoneResidual>`.
- `Result<T,E>` implementiert `Propagate<T,E>` und `FromResidual<E>`.
- `Result<T,E>` erhält zusätzlich abgeleitet `FromResidual<R>`, wenn `E` strukturell `FromResidual<R>` erfüllt. So
  konvertiert etwa `AppError.fromResidual(IOError)` den Fehler explizit.
- Zwischen den Standardtypen wird nicht gekreuzt: `Option?` kann nicht in `Result` und `Result?` nicht in `Option`
  propagieren. Benutzerdefinierte Typen dürfen bewusst beliebige Residuals akzeptieren.

## Compiler-Änderungen

- Lexer, Parser, AST und Dumps um `TOKEN_QUESTION` und `AST_EXPR_PROPAGATE` erweitern. `expression?` ist ein
  Postfixoperator mit Call-/Member-/Index-Präzedenz; Verkettungen wie `read()?.field` und `nested??` sind erlaubt.
- Die Semantik ermittelt eine eindeutige `Propagate<Output,Residual>`-Spezialisierung des Operanden und prüft
  `FromResidual<Residual>` am aktuellen Rückgabetyp. Das Ergebnis des Ausdrucks hat Typ `Output`.
- `?` ist nur in Funktionen und Methoden erlaubt; in Konstanten, Package-Initializern, Destruktoren und Deferred
  Closures wird es gezielt abgelehnt.
- Der Operand wird wie ein by-value Funktionsargument behandelt: copyable Werte bleiben nutzbar, move-only Container
  werden konsumiert. Für move-only Enum-Payloads wird `match` um konsumierende Payload-Bindings erweitert, damit
  `branch` Output oder Residual ohne Kopie bewegen kann.
- Die semantische Auflösung speichert Branch-, Continue-/Break- und FromResidual-Ziele vollständig im AST;
  IR-Lowering führt keine erneute Namensauflösung durch.
- IR-Lowering erzeugt einen Branch auf `Propagation.Continue`/`Break`. Der Continue-Pfad extrahiert den Output; der
  Break-Pfad extrahiert den Residual, ruft die aufgelöste Konvertierung auf und nutzt denselben Cleanup-/Return-Pfad wie
  ein explizites `return`.
- Temporäre Container und Payloads werden auf jedem Pfad genau einmal bewegt oder zerstört. Bei früher Rückgabe laufen
  aktive `defer`s in LIFO-Reihenfolge und lokale beziehungsweise Parameter-Drops wie bei normalem `return`; `exit`/`trap`
  bleiben davon unberührt.
- Verschachtelte `?` respektieren Links-nach-rechts-Auswertung: Nach einem Fehler werden spätere Operanden oder Argumente
  nicht mehr ausgewertet.
- Diagnosen zeigen primär auf `?` und unterscheiden fehlendes/mehrdeutiges `Propagate`, unzulässigen Kontext,
  inkompatiblen Rückgabetyp, fehlendes `FromResidual` und verbotene Option/Result-Kreuzung.

## Stdlib, Dokumentation und Beispiele

- `Option`, `Result`, `Propagation` und `NoneResidual` werden in `stdlib/foundation.dmm` mit den statischen
  Contract-Methoden implementiert.
- Ein Beispiel zeigt direkte Result-Propagation, `IOError -> AppError`, Option-Propagation und einen benutzerdefinierten
  Typ wie `Validation<T>`.
- Sprachspezifikation, formale Grammatik, Ownership-/Cleanup-Regeln und TODO werden aktualisiert; bestehende
  `Option`-/`Result`-Konstruktoren bleiben quellkompatibel.

## Testplan

- Erfolg und frühe Rückgabe für `Option`, `Result` und einen benutzerdefinierten Propagationstyp.
- Gleicher Result-Fehlertyp sowie explizite Konvertierung über `E:FromResidual<R>`.
- Ablehnung beider Option/Result-Kreuzrichtungen und fehlender beziehungsweise mehrdeutiger Contracts.
- Verkettetes und verschachteltes `?`, Memberzugriff nach `?` und mehrere `?` in Call-Argumenten mit geprüfter
  Auswertungsreihenfolge.
- Move-only Output und Residual, konsumierter Operand, Borrow-Konflikte sowie exakt einmal ausgeführte Destruktoren.
- Frühe Rückgabe mit mehreren `defer`s, lokalen Owners und owning Parametern; Reihenfolge und genau einmal ausgeführtes
  Cleanup prüfen.
- Separate Tests für generische Interfaces, statische Interface-Anforderungen, Enum-Methoden und konsumierende
  Match-Bindings.
- IR-Verifikation und Laufzeittests unter O0/O1, Intel/AT&T sowie Windows- und Linux-Clang-CI; anschließend vollständiges
  `ctest`.

## Annahmen

- Generische Interface-Parameter repräsentieren `Output` und `Residual`; echte Associated-Type-Projektionen und
  `impl`-Blöcke werden nicht zusätzlich eingeführt.
- Fehlerkonvertierung erfolgt ausschließlich über `FromResidual`, nie über allgemeine implizite Typkonvertierungen.
- `?` führt normalen frühen Return aus, kein Exception-Unwinding.
- Try-Blöcke, Catch-Syntax, Async-Propagation und Ausdrucksformen für `if`/`match` bleiben außerhalb dieses Vorhabens.
