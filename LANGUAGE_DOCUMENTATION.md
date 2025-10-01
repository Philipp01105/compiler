```markdown
# Programmiersprache Dokumentation

**Version:** 1.0  
**Datum:** 2025-01-14  
**Compiler:** Custom C Compiler für Windows x64

---

## Inhaltsverzeichnis

1. [Übersicht](#übersicht)
2. [Datentypen](#datentypen)
3. [Variablen](#variablen)
4. [Operatoren](#operatoren)
5. [Kontrollstrukturen](#kontrollstrukturen)
6. [Funktionen](#funktionen)
7. [Ein-/Ausgabe](#ein-ausgabe)
8. [Kommentare](#kommentare)
9. [Code-Style Konventionen](#code-style-konventionen)
10. [Beispiele](#beispiele)
11. [Compiler-Verwendung](#compiler-verwendung)

---

## Übersicht

Eine einfache, statisch typisierte Programmiersprache mit C-ähnlicher Syntax, die zu x86-64 Assembly kompiliert wird.

### Hauptmerkmale

- ✅ Statische Typisierung
- ✅ Funktionen mit Typannotationen
- ✅ For-Schleifen
- ✅ Arithmetische und Vergleichsoperatoren
- ✅ String-Konkatenation in Print-Statements
- ✅ Scope-basierte Variablen-Verwaltung

---

## Datentypen

### Unterstützte Typen

| Typ | Beschreibung | Beispiel |
|-----|--------------|----------|
| `int` | 32-Bit Ganzzahl | `42`, `-17`, `0` |
| `string` | Zeichenkette (zur Zeit nur in Literalen) | `"Hello World"` |
| `void` | Kein Rückgabewert (nur für Funktionen) | - |

### Typ-Eigenschaften

- **Unveränderlich**: Variablen können ihren Typ nach der Deklaration nicht ändern
- **Type Inference**: Der Typ wird bei der Initialisierung automatisch abgeleitet

```javascript
var x = 10;        // x ist int
var name = "test"; // name ist string (noch nicht vollständig implementiert)
```

---

## Variablen

### Deklaration

**Syntax:**
```javascript
var name = wert;
```

**Regeln:**
- Variablen **müssen** mit `var` deklariert werden
- Variablen **müssen** bei der Deklaration initialisiert werden
- Variablennamen dürfen nur einmal im selben Scope deklariert werden
- Variablen sind **scope-gebunden** (Funktions-Scope)

**Beispiele:**
```javascript
var x = 10;
var counter = 0;
var result = x + 5;
```

**Fehler:**
```javascript
var x;           // ❌ Fehler: Muss initialisiert werden
x = 10;          // ❌ Fehler: Variable nicht deklariert (var fehlt)
var x = 10;
var x = 20;      // ❌ Fehler: Variable bereits deklariert
```

### Zuweisung

**Syntax:**
```javascript
name = wert;
```

**Beispiel:**
```javascript
var x = 10;
x = 20;          // ✅ OK
x = x + 5;       // ✅ OK
```

---

## Operatoren

### Arithmetische Operatoren

| Operator | Beschreibung | Beispiel |
|----------|--------------|----------|
| `+` | Addition | `a + b` |
| `-` | Subtraktion | `a - b` |
| `*` | Multiplikation | `a * b` |
| `/` | Division | `a / b` |

**Beispiel:**
```javascript
var a = 10;
var b = 5;
var sum = a + b;        // 15
var diff = a - b;       // 5
var prod = a * b;       // 50
var quot = a / b;       // 2
```

### Compound Assignment Operatoren

| Operator | Beschreibung | Äquivalent |
|----------|--------------|------------|
| `+=` | Addition und Zuweisung | `x = x + y` |
| `-=` | Subtraktion und Zuweisung | `x = x - y` |
| `*=` | Multiplikation und Zuweisung | `x = x * y` |
| `/=` | Division und Zuweisung | `x = x / y` |

**Beispiel:**
```javascript
var x = 10;
x += 5;    // x = 15
x -= 3;    // x = 12
x *= 2;    // x = 24
x /= 4;    // x = 6
```

### Vergleichsoperatoren

| Operator | Beschreibung | Beispiel |
|----------|--------------|----------|
| `<` | Kleiner als | `a < b` |
| `>` | Größer als | `a > b` |
| `<=` | Kleiner oder gleich | `a <= b` |
| `>=` | Größer oder gleich | `a >= b` |
| `==` | Gleich | `a == b` |
| `!=` | Ungleich | `a != b` |

**Beispiel:**
```javascript
var a = 10;
var b = 20;

// Verwendet in for-Schleifen
for(var i = 0; i < 10; i += 1) { }     // i < 10
for(var i = 0; i <= 10; i += 1) { }    // i <= 10
```

### Operator-Priorität

1. `*`, `/` (höchste Priorität)
2. `+`, `-`
3. `<`, `>`, `<=`, `>=`
4. `==`, `!=` (niedrigste Priorität)

---

## Kontrollstrukturen

### For-Schleife

**Syntax:**
```javascript
for(initialisierung; bedingung; inkrement) {
    // body
}
```

**Komponenten:**
- **Initialisierung**: Wird einmal vor der Schleife ausgeführt
- **Bedingung**: Wird vor jeder Iteration geprüft
- **Inkrement**: Wird nach jeder Iteration ausgeführt

**Beispiel:**
```javascript
for(var i = 0; i < 10; i += 1) {
    print("i = " + i);
}
```

**Verschachtelte Schleifen:**
```javascript
for(var i = 1; i <= 3; i += 1) {
    for(var j = 1; j <= 3; j += 1) {
        print("i = " + i + ", j = " + j);
    }
}
```

**Wichtig:**
- Klammern `{}` sind **erforderlich**
- Schleifenvariable ist im Schleifenkörper verfügbar
- Schleifenvariable sollte mit `var` in der Initialisierung deklariert werden

---

## Funktionen

### Funktionsdeklaration

**Syntax:**
```javascript
func name(param1:typ1, param2:typ2, ...) -> rückgabetyp {
    // body
    return wert;
}
```

**Regeln:**
- Funktionen werden mit `func` eingeleitet
- Parameter müssen **Typannotationen** haben (`name:typ`)
- Rückgabetyp wird mit `->` angegeben
- Rückgabetyp ist **erforderlich** (verwende `void` wenn kein Wert zurückgegeben wird)

**Beispiele:**

```javascript
// Einfache Funktion
func add(a:int, b:int) -> int {
    return a + b;
}

// Funktion mit mehreren Parametern
func multiply(x:int, y:int) -> int {
    return x * y;
}

// Funktion ohne Rückgabewert
func printNumber(n:int) -> void {
    print("Number: " + n);
}

// Funktion mit lokalen Variablen
func factorial(n:int) -> int {
    var result = 1;
    for(var i = 1; i <= n; i += 1) {
        result *= i;
    }
    return result;
}
```

### Funktionsaufruf

**Syntax:**
```javascript
funktionsname(arg1, arg2, ...);
```

**Beispiele:**
```javascript
var sum = add(5, 7);
var product = multiply(3, 4);
printNumber(42);

var fact = factorial(5);
print("5! = " + fact);
```

### Return-Statement

**Syntax:**
```javascript
return ausdruck;
```

**Regeln:**
- Return gibt den Wert zurück und beendet die Funktion sofort
- Bei `void`-Funktionen kann `return;` verwendet werden (ohne Wert)
- Fehlender return bei `int`-Funktionen gibt automatisch `0` zurück

**Beispiele:**
```javascript
func max(a:int, b:int) -> int {
    // Bedingte Rückgabe basierend auf Vergleich
    var result = a;
    if (b > a) {
        result = b;
    }
    return result;
}
```

---

## Ein-/Ausgabe

### Print-Funktion

**Syntax:**
```javascript
print(ausdruck);
```

**Features:**
- Gibt Werte auf der Konsole aus
- Automatischer Zeilenumbruch am Ende
- Unterstützt String-Konkatenation mit `+`
- Kann Strings und Integer mischen

**Beispiele:**

```javascript
// Einfache Ausgabe
print("Hello World");

// Variablen ausgeben
var x = 10;
print("x = " + x);

// Mehrere Werte kombinieren
var a = 5;
var b = 7;
print("a = " + a + ", b = " + b);

// Berechnungen in print
print("Summe: " + (a + b));

// Escape-Sequenzen
print("\n=== Überschrift ===\n");
```

**Wichtig:**
- String-Literale müssen in **doppelten Anführungszeichen** stehen
- Verkettung mit `+` funktioniert nur in print-Statements

---

## Kommentare

### Einzeilige Kommentare

**Syntax:**
```javascript
// Dies ist ein Kommentar
```

**Beispiele:**
```javascript
// Berechne die Summe
var sum = a + b;

var x = 10;  // Initialisiere x mit 10

// Dies ist ein mehrzeiliger Kommentar
// der über mehrere Zeilen geht
// und detaillierte Erklärungen enthält
```

**Wichtig:**
- Nur `//` wird unterstützt (keine `/* */` Kommentare)
- Kommentare können am Zeilenende oder auf separaten Zeilen stehen

---

## Code-Style Konventionen

### Empfohlener Stil

#### 1. Klammern auf neuer Zeile

**✅ Empfohlen:**
```javascript
func add(a:int, b:int) -> int
{
    return a + b;
}

for(var i = 0; i < 10; i += 1)
{
    print(i);
}
```

**⚠️ Funktioniert, aber nicht empfohlen:**
```javascript
func add(a:int, b:int) -> int {
    return a + b;
}
```

Der Compiler gibt eine Warnung aus:
```
⚠️  Warnung (Zeile 1): Öffnende Klammer '{' sollte auf neuer Zeile stehen.
   Empfohlener Stil:
   func name(params) -> type
   {
       ...
   }
```

#### 2. Einrückung

**Empfehlung:** 4 Spaces

```javascript
func factorial(n:int) -> int
{
    var result = 1;
    for(var i = 1; i <= n; i += 1)
    {
        result *= i;
    }
    return result;
}
```

#### 3. Leerzeichen

**Empfehlung:**
- Leerzeichen um Operatoren: `a + b`, nicht `a+b`
- Leerzeichen nach Kommas: `func(a, b, c)`, nicht `func(a,b,c)`
- Kein Leerzeichen vor Semikolon: `var x = 10;`, nicht `var x = 10 ;`

#### 4. Namenskonventionen

**Variablen und Funktionen:**
- camelCase: `myVariable`, `calculateSum`

**Konstanten (wenn implementiert):**
- UPPER_SNAKE_CASE: `MAX_VALUE`, `PI`

---

## Beispiele

### Vollständiges Programm 1: Grundlagen

```javascript
// Einfaches Programm mit Variablen und Ausgabe

var x = 10;
var y = 20;
var sum = x + y;

print("=== Rechnung ===");
print("x = " + x);
print("y = " + y);
print("Summe = " + sum);
```

### Vollständiges Programm 2: Funktionen

```javascript
// Programm mit Funktionen

func add(a:int, b:int) -> int
{
    return a + b;
}

func multiply(x:int, y:int) -> int
{
    return x * y;
}

var a = 5;
var b = 7;

var sum = add(a, b);
var product = multiply(a, b);

print("a = " + a);
print("b = " + b);
print("Summe = " + sum);
print("Produkt = " + product);
```

### Vollständiges Programm 3: Schleifen

```javascript
// Programm mit For-Schleife

print("=== Zahlen von 1 bis 10 ===");

for(var i = 1; i <= 10; i += 1)
{
    print("Zahl: " + i);
}

print("\n=== Quadratzahlen ===");

for(var i = 1; i <= 5; i += 1)
{
    var square = i * i;
    print(i + " ^ 2 = " + square);
}
```

### Vollständiges Programm 4: Fakultät

```javascript
// Fakultät berechnen

func factorial(n:int) -> int
{
    var result = 1;
    for(var i = 1; i <= n; i += 1)
    {
        result *= i;
    }
    return result;
}

print("=== Fakultäten ===");

for(var i = 1; i <= 10; i += 1)
{
    var fact = factorial(i);
    print(i + "! = " + fact);
}
```

### Vollständiges Programm 5: Fibonacci

```javascript
// Fibonacci-Zahlen

func fibonacci(n:int) -> int
{
    var a = 0;
    var b = 1;
    
    for(var i = 0; i < n; i += 1)
    {
        var temp = a + b;
        a = b;
        b = temp;
    }
    
    return a;
}

print("=== Fibonacci-Folge ===");

for(var i = 0; i < 15; i += 1)
{
    var fib = fibonacci(i);
    print("F(" + i + ") = " + fib);
}
```

### Vollständiges Programm 6: Primzahlen

```javascript
// Primzahlen finden

func isPrime(n:int) -> int
{
    if (n <= 1)
    {
        return 0;  // false
    }
    
    for(var i = 2; i < n; i += 1)
    {
        var remainder = n - (n / i) * i;  // Modulo-Operation
        if (remainder == 0)
        {
            return 0;  // false
        }
    }
    
    return 1;  // true
}

print("=== Primzahlen bis 30 ===");

for(var i = 2; i <= 30; i += 1)
{
    var prime = isPrime(i);
    if (prime == 1)
    {
        print(i + " ist eine Primzahl");
    }
}
```

---

## Compiler-Verwendung

### Installation

```bash
# Compiler kompilieren
gcc compiler.c -o compiler.exe
```

### Grundlegende Verwendung

```bash
# Programm kompilieren
./compiler.exe program.txt

# Assembly-Datei wird erstellt: program.txt.s
gcc program.txt.s -o program.exe

# Programm ausführen
./program.exe
```

### Debug-Modus

```bash
# Mit detaillierten Debug-Informationen
./compiler.exe --debug program.txt

# Debug-Ausgabe in Datei speichern
./compiler.exe --debug program.txt 2> debug.log
```

### Compiler-Ausgabe

**Erfolgreiche Kompilierung:**
```
=================================
✅ Kompilierung erfolgreich!
=================================
📄 Zeilen: 42
🔢 Main Variablen: 5
🔤 Strings: 8
⚙️  Funktionen: 3

📝 Assembly: program.txt.s

🔨 Kompilieren: gcc program.txt.s -o program
▶️  Ausführen:  ./program
=================================
```

**Fehler:**
```
Fehler: Variable 'x' nicht deklariert!
```

**Warnung:**
```
⚠️  Warnung (Zeile 1): Öffnende Klammer '{' sollte auf neuer Zeile stehen.
```

### Dateiformat

- **Encoding:** UTF-8 oder ASCII
- **Zeilenenden:** Unix (LF) oder Windows (CRLF) - beide werden unterstützt
- **Dateiendung:** `.txt` (beliebig, aber empfohlen)

---

## Einschränkungen und bekannte Probleme

### Aktuelle Einschränkungen

1. **Keine If-Else Statements** (nur bedingte Logik über Vergleiche in for-Schleifen)
2. **Keine While/Do-While Schleifen** (nur for)
3. **Keine Arrays** (nur einzelne Variablen)
4. **Keine Strings als Variablen** (nur als Literale in print)
5. **Keine Structs/Klassen**
6. **Keine Pointer**
7. **Keine dynamische Speicherverwaltung**
8. **Maximal 4 Funktionsparameter** (Windows x64 calling convention)
9. **Keine echte Rekursion** (Stack-Limitierung)
10. **Keine globalen Variablen** (nur in Funktionen und Main-Bereich)
11. **Kein Modulo-Operator** (muss manuell berechnet werden: `n - (n / d) * d`)
12. **Keine Boolean Typ** (int als 0/1 verwenden)

### Geplante Features

- ✨ If-Else Statements
- ✨ While-Schleifen
- ✨ Arrays
- ✨ String-Variablen
- ✨ Boolean Typ
- ✨ Logische Operatoren (&&, ||, !)
- ✨ Modulo-Operator (%)
- ✨ Break/Continue in Schleifen
- ✨ Mehr als 4 Funktionsparameter
- ✨ Switch-Case Statements

---

## Fehlerbehandlung

### Compiler-Fehler

| Fehler | Ursache | Lösung |
|--------|---------|--------|
| `Variable 'x' nicht deklariert!` | Variable verwendet ohne Deklaration | Mit `var x = wert;` deklarieren |
| `Variable 'x' bereits deklariert!` | Variable zweimal im gleichen Scope deklariert | Anderen Namen wählen oder nur zuweisen |
| `Variable muss initialisiert werden!` | `var x;` ohne Zuweisung | `var x = wert;` verwenden |
| `Funktion braucht Rückgabetyp (-> type)` | `->` fehlt in Funktionsdefinition | `-> typ` hinzufügen |
| `Parameter braucht Typ (name:type)` | Parameter ohne Typannotation | `:typ` nach Parametername |
| `Addition nur mit int möglich!` | Typ-Konflikt bei Operation | Typen prüfen |

### Runtime-Fehler

| Fehler | Ursache | Lösung |
|--------|---------|--------|
| Division durch 0 | `x / 0` | Bedingung prüfen vor Division |
| Stack Overflow | Zu tiefe Rekursion oder zu viele lokale Variablen | Rekursionstiefe reduzieren |

---

## Compiler-Flags und Optionen

### Verfügbare Flags

| Flag | Beschreibung |
|------|--------------|
| `--debug` | Aktiviert detaillierte Debug-Ausgabe während der Kompilierung |

### Beispiele

```bash
# Normal
./compiler.exe program.txt

# Mit Debug
./compiler.exe --debug program.txt

# Debug in Datei
./compiler.exe --debug program.txt 2> debug.log
cat debug.log
```

---

## Technische Details

### Zielplattform

- **Architektur:** x86-64 (64-bit)
- **Betriebssystem:** Windows
- **Calling Convention:** Microsoft x64 calling convention
- **Assembler:** GNU Assembler (GAS)

### Generierte Assembly

- **Format:** AT&T Syntax
- **Register-Verwendung:**
    - `%rax`: Return-Wert, temporäre Berechnungen
    - `%rbx`: Temporäre Berechnungen
    - `%rcx, %rdx, %r8, %r9`: Funktionsparameter 1-4
    - `%rbp`: Frame Pointer
    - `%rsp`: Stack Pointer

### Speicher-Layout

- **Stack-Alignment:** 16 Bytes
- **Variablen-Größe:** 4 Bytes (int)
- **Stack-Frame:** 256 Bytes pro Funktion

---

## FAQ

### Warum funktionieren meine Funktionen nicht?

**Problem:** Klammer `{` in der gleichen Zeile wie die Funktionsdefinition.

**Lösung:**
```javascript
// ❌ Nicht empfohlen (gibt Warnung)
func add(a:int, b:int) -> int {

// ✅ Empfohlen
func add(a:int, b:int) -> int
{
```

### Kann ich globale Variablen verwenden?

**Nein.** Aktuell werden nur lokale Variablen in Funktionen und im Main-Bereich unterstützt. Variablen im Main-Bereich sind quasi "global" für die Main-Funktion.

### Wie viele Parameter kann eine Funktion haben?

**Maximal 4 Parameter** aufgrund der Windows x64 Calling Convention (Register: `rcx, rdx, r8, r9`).

### Unterstützt der Compiler Rekursion?

**Theoretisch ja**, aber es gibt Stack-Limitierungen. Tiefe Rekursion kann zu Stack Overflow führen. Besser iterative Lösungen verwenden.

### Kann ich Arrays verwenden?

**Nein.** Arrays sind aktuell nicht implementiert.

### Wie kann ich Strings speichern?

**Aktuell nicht möglich.** Strings können nur als Literale in `print()` verwendet werden.

### Wie berechne ich Modulo (Rest der Division)?

**Manuell berechnen:**
```javascript
// n modulo d = n - (n / d) * d
var n = 17;
var d = 5;
var remainder = n - (n / d) * d;  // remainder = 2
```

### Wie implementiere ich If-Else?

**Workaround:** Verwende bedingte Zuweisungen oder mehrere Returns:
```javascript
func max(a:int, b:int) -> int
{
    var result = a;
    var diff = b - a;
    
    // Wenn diff > 0, dann ist b größer
    // Primitive if-Simulation über Schleifen
    for(var i = 0; i < 1; i += 1)
    {
        if (b > a)
        {
            result = b;
        }
    }
    
    return result;
}
```

---

## Lizenz und Credits

**Entwickler:** Philipp  
**Version:** 1.0  
**Datum:** 2025-10-14

---

## Changelog

### Version 1.0 (2025-01-14)
- ✅ Initiale Version
- ✅ Variablen mit `var` Keyword
- ✅ Funktionen mit Typannotationen (`func name(a:int) -> int`)
- ✅ For-Schleifen
- ✅ Arithmetische Operatoren (`+`, `-`, `*`, `/`)
- ✅ Vergleichsoperatoren (`<`, `>`, `<=`, `>=`, `==`, `!=`)
- ✅ Compound Assignment Operatoren (`+=`, `-=`, `*=`, `/=`)
- ✅ Print-Funktion mit String-Konkatenation
- ✅ CRLF/LF-Kompatibilität
- ✅ Debug-Modus (`--debug` Flag)
- ✅ Warnungen für Code-Style
- ✅ Scope-basierte Variablen-Verwaltung

---

## Anhang

### Grammatik (BNF-ähnlich)

```
program       ::= (function | statement)*

function      ::= "func" identifier "(" params ")" "->" type "{" statement* "}"
params        ::= param ("," param)*
param         ::= identifier ":" type

statement     ::= var_decl | assignment | for_loop | return | func_call | print

var_decl      ::= "var" identifier "=" expression ";"
assignment    ::= identifier op "=" expression ";"
              | identifier "=" expression ";"
op            ::= "+" | "-" | "*" | "/"

for_loop      ::= "for" "(" var_decl ";" expression ";" assignment ")" "{" statement* "}"

return        ::= "return" expression? ";"

func_call     ::= identifier "(" args ")" ";"
args          ::= expression ("," expression)*

print         ::= "print" "(" print_expr ")" ";"
print_expr    ::= string | expression | print_expr "+" print_expr

expression    ::= term
              | expression "+" term
              | expression "-" term

term          ::= factor
              | term "*" factor
              | term "/" factor

factor        ::= number
              | identifier
              | "(" expression ")"

comparison    ::= expression "<" expression
              | expression ">" expression
              | expression "<=" expression
              | expression ">=" expression
              | expression "==" expression
              | expression "!=" expression

type          ::= "int" | "string" | "void"
identifier    ::= [a-zA-Z_][a-zA-Z0-9_]*
number        ::= [0-9]+
string        ::= '"' [^"]* '"'
```

### Reservierte Wörter

- `var` - Variablendeklaration
- `func` - Funktionsdeklaration
- `return` - Return-Statement
- `for` - For-Schleife
- `print` - Ausgabe-Funktion
- `int` - Integer-Typ
- `string` - String-Typ
- `void` - Void-Typ

### Escape-Sequenzen in Strings

| Sequenz | Bedeutung |
|---------|-----------|
| `\n` | Zeilenumbruch (Newline) |
| `\"` | Doppeltes Anführungszeichen |
| `\\` | Backslash |

**Beispiel:**
```javascript
print("Zeile 1\nZeile 2");
print("Er sagte: \"Hallo\"");
print("Pfad: C:\\Users\\Name");
```

---

**Ende der Dokumentation**
```

Du kannst diese Markdown-Datei speichern als `LANGUAGE_DOCUMENTATION.md` und sie wird korrekt formatiert angezeigt in jedem Markdown-Viewer (GitHub, GitLab, VSCode, etc.). 📝✨