# Compiler Dokumentation - Version 1.3

**Autor:** Philipp01105  
**Datum:** 2025-01-14  
**Version:** 1.3 (Stable Release)

---

## Inhaltsverzeichnis

1. [Überblick](#überblick)
2. [Features](#features)
3. [Installation](#installation)
4. [Sprachsyntax](#sprachsyntax)
5. [Beispiele](#beispiele)
6. [Architektur](#architektur)
7. [Bekannte Einschränkungen](#bekannte-einschränkungen)
8. [Changelog](#changelog)
9. [FAQ](#faq)

---

## Überblick

Dies ist ein **vollständiger Compiler** für eine eigene Programmiersprache, die zu **x86-64 Assembly** (AT&T-Syntax) kompiliert wird. Der Compiler ist in C geschrieben und generiert ausführbaren Code für **Windows (MinGW-w64)**.

### Kernmerkmale

- ✅ **Funktionen** mit Parametern und Rückgabewerten
- ✅ **Variablen** (int-Typ)
- ✅ **Arithmetische Operatoren** (+, -, *, /)
- ✅ **Vergleichsoperatoren** (<, <=, >, >=, ==, !=)
- ✅ **Compound Assignment** (+=, -=, *=, /=)
- ✅ **Inkrement/Dekrement** (++, --)
- ✅ **For-Schleifen** mit Scope-Management
- ✅ **Verschachtelte Schleifen**
- ✅ **Klammer-Unterstützung** in Expressions
- ✅ **String-Konkatenation** mit `print()`
- ✅ **Verschachtelte Funktionsaufrufe**
- ✅ **Debug-Modus** für detaillierte Compiler-Ausgaben

---

## Features

### 1. Funktionen

```javascript
// Funktion mit Parametern und Rückgabewert
func add(a:int, b:int) -> int
{
  return a + b;
}

// Funktion ohne Parameter
func getConstant() -> int
{
  return 42;
}

// Funktion ohne Rückgabewert (void)
func printSeparator() -> void
        {
          print("=================================");
}
```

**Obligatorisch:** Jedes Programm **muss** eine `main()` Funktion haben:

```javascript
func main() -> void
        {
          print("Hello World!");
}
```

### 2. Variablen

```javascript
var x = 10;
var y = 20;
var result = x + y;
```

- Variablen müssen **immer initialisiert** werden
- Aktuell nur **int**-Typ unterstützt
- Type Inference (Typ wird automatisch erkannt)

### 3. Operatoren

#### Arithmetische Operatoren

```javascript
var a = 10;
var b = 3;

var sum = a + b;      // 13
var diff = a - b;     // 7
var prod = a * b;     // 30
var quot = a / b;     // 3 (Integer-Division)
```

#### Vergleichsoperatoren

```javascript
var isLess = a < b;       // 0 (false)
var isGreater = a > b;    // 1 (true)
var isEqual = a == b;     // 0 (false)
var isNotEqual = a != b;  // 1 (true)
var isLessEq = a <= b;    // 0 (false)
var isGreaterEq = a >= b; // 1 (true)
```

#### Compound Assignment

```javascript
var x = 10;
x += 5;  // x = 15
x -= 3;  // x = 12
x *= 2;  // x = 24
x /= 4;  // x = 6
```

#### Inkrement/Dekrement

```javascript
for(var i = 0; i < 10; i++)
{
  print("i = " + i);
}

for(var j = 10; j > 0; j--)
{
  print("j = " + j);
}
```

### 4. For-Schleifen

```javascript
// Grundform
for(var i = 0; i < 10; i++)
{
  print("i = " + i);
}

// Mit vorheriger Deklaration
var j = 0;
for(j = 0; j < 5; j++)
{
  print("j = " + j);
}

// Verschachtelt
for(var i = 0; i < 3; i++)
{
  for(var j = 0; j < 3; j++)
  {
    var product = i * j;
    print("i * j = " + product);
  }
}

// Mit Schrittweite
for(var n = 0; n < 10; n += 2)
{
  print("n = " + n);
}
```

**Wichtig:** Variablen innerhalb von Schleifen haben **Loop-Scope** und werden nach der Schleife automatisch aufgeräumt!

### 5. Print-Statement

```javascript
// Einfacher String
print("Hello World");

// String + Variable
var x = 42;
print("x = " + x);

// Mehrfache Konkatenation
print("x = " + x + ", y = " + y);

// Mit Berechnungen
print("Summe: " + (x + y));
```

### 6. Klammern in Expressions

```javascript
var result = (a + b) * c;           // ✅ Funktioniert
var complex = (x * (y + z)) / 2;    // ✅ Funktioniert
var nested = ((a + b) * (c - d));   // ✅ Funktioniert
```

### 7. Verschachtelte Funktionsaufrufe

```javascript
var result = add(10, multiply(2, 3));          // ✅ Funktioniert
var complex = divide(add(a, b), subtract(c, d)); // ✅ Funktioniert
```

---

## Installation

### Voraussetzungen

- **GCC (MinGW-w64)** für Windows
- **Make** (optional)
- **Text-Editor** oder IDE

### Compiler kompilieren

```bash
gcc compiler.c -o compiler.exe
```

### Programm kompilieren und ausführen

```bash
# 1. Source-Code zu Assembly
./compiler.exe program.txt

# 2. Assembly zu Executable
gcc program.txt.s -o program.exe

# 3. Programm ausführen
./program.exe
```

### Debug-Modus

```bash
./compiler.exe --debug program.txt 2> debug.log
```

---

## Sprachsyntax

### Funktionsdefinition

```
func <name>(<param>:<type>, ...) -> <return_type>
{
    <body>
}
```

**Beispiel:**
```javascript
func factorial(n:int) -> int
{
  var result = 1;
  for(var i = 1; i <= n; i++)
  {
    result *= i;
  }
  return result;
}
```

### Variablendeklaration

```
var <name> = <expression>;
```

**Beispiel:**
```javascript
var x = 10;
var sum = a + b;
var result = multiply(x, y);
```

### For-Schleife

```
for(<init>; <condition>; <increment>)
{
    <body>
}
```

**Beispiel:**
```javascript
for(var i = 0; i < 10; i++)
{
  print("i = " + i);
}
```

### Print-Statement

```
print(<expression>);
```

**Beispiel:**
```javascript
print("Hello World");
print("x = " + x);
print("Sum: " + (a + b));
```

### Return-Statement

```
return <expression>;
```

**Beispiel:**
```javascript
return a + b;
return factorial(n - 1) * n;
```

### Kommentare

```javascript
// Einzeilige Kommentare werden unterstützt
var x = 10; // Auch am Ende einer Zeile
```

---

## Beispiele

### Hello World

```javascript
func main() -> void
        {
          print("Hello World!");
}
```

### Fakultät

```javascript
func factorial(n:int) -> int
{
  var result = 1;
  for(var i = 1; i <= n; i++)
  {
    result *= i;
  }
  return result;
}

func main() -> void
        {
          var fact = factorial(5);
          print("5! = " + fact);
}
```

### Fibonacci

```javascript
func fibonacci(n:int) -> int
{
  var a = 0;
  var b = 1;

  for(var i = 0; i < n; i++)
  {
    var temp = a + b;
    a = b;
    b = temp;
  }

  return a;
}

func main() -> void
        {
          print("Fibonacci-Zahlen:");
for(var i = 0; i < 10; i++)
{
  var fib = fibonacci(i);
  print("F(" + i + ") = " + fib);
}
}
```

### GGT (Größter Gemeinsamer Teiler)

```javascript
func modulo(n:int, d:int) -> int
{
  var quotient = n / d;
  var product = quotient * d;
  var remainder = n - product;
  return remainder;
}

func gcd(a:int, b:int) -> int
{
  for(var i = 0; i < 50; i++)
  {
    var bNotZero = 0;
    for(var check = 0; check < b; check++)
    {
      bNotZero = 1;
    }

    for(var calc = 0; calc < bNotZero; calc++)
    {
      var temp = b;
      b = modulo(a, b);
      a = temp;
    }
  }

  return a;
}

func main() -> void
        {
          var result = gcd(48, 18);
          print("gcd(48, 18) = " + result);
}
```

---

## Architektur

### Compiler-Pipeline

```
Source Code (.txt)
    ↓
Lexer/Parser (Zeile für Zeile)
    ↓
Variable Management (Scope-basiert)
    ↓
Code Generation (x86-64 Assembly)
    ↓
Assembly Output (.s)
    ↓
GCC Assembler/Linker
    ↓
Executable (.exe)
```

### Wichtige Komponenten

#### 1. Variable Management

```c
typedef struct {
    char name[MAX_TOKEN];
    int offset;        // Stack-Offset
    int scope;         // Scope-Level
    DataType type;     // Datentyp
} Variable;
```

- Variablen werden als Stack-Frame-Offsets verwaltet
- Scope-basiertes Tracking (Global=0, Funktion=1, Loop=2+)
- Automatisches Cleanup nach Scope-Ende

#### 2. Function Registry

```c
typedef struct {
    char name[MAX_TOKEN];
    int param_count;
    char params[10][MAX_TOKEN];
    DataType param_types[10];
    DataType return_type;
} Function;
```

- Funktionen werden beim ersten Pass registriert
- Parameter-Typen und Anzahl werden gespeichert
- Unterstützt bis zu 10 Parameter

#### 3. Expression Parser

- **Operator Precedence:**
  1. Klammern `()`
  2. Multiplikation `*`, Division `/`
  3. Addition `+`, Subtraktion `-`
  4. Vergleiche `<`, `<=`, `>`, `>=`, `==`, `!=`

- **Rekursiver Descent Parser**
- **Klammer-bewusstes Parsing**

#### 4. Code Generation

- **AT&T Syntax** für GAS (GNU Assembler)
- **Windows x64 Calling Convention**
- **Stack-basierte Berechnung**
- **32-Byte Shadow Space** für Funktionsaufrufe

### Stack Layout

```
High Address
│
├─ Return Address
├─ Saved RBP          ← RBP points here
├─ Local Variable 1   [rbp-4]
├─ Local Variable 2   [rbp-8]
├─ Local Variable 3   [rbp-12]
│  ...
├─ Local Variable N   [rbp-(N*4)]
└─ Stack Reserve      [rbp-2048]
   (2048 Bytes)
Low Address
```

### Register Usage

- **RAX:** Accumulator, Return-Wert
- **RBX:** Temporary für Operationen
- **RCX:** 1. Parameter
- **RDX:** 2. Parameter
- **R8:** 3. Parameter
- **R9:** 4. Parameter
- **RBP:** Frame Pointer
- **RSP:** Stack Pointer

---

## Bekannte Einschränkungen

### Datentypen

- ❌ Aktuell nur **int** (32-Bit) unterstützt
- ❌ Keine Strings als Variablentyp (nur in `print()`)
- ❌ Keine Floats/Doubles
- ❌ Keine Arrays
- ❌ Keine Structs

### Kontrollstrukturen

- ❌ Keine `if`-Statements (Workaround: For-Schleifen als Conditionals)
- ❌ Keine `while`-Schleifen
- ❌ Keine `switch`-Statements
- ❌ Kein `break` oder `continue`

### Funktionen

- ✅ Maximal **10 Parameter** pro Funktion
- ❌ Keine Rekursion möglich (Stack würde überlaufen)
- ❌ Keine Function Pointers
- ❌ Keine Closures

### Variablen

- ✅ Maximal **200 Variablen** gleichzeitig
- ❌ Keine globalen Variablen
- ❌ Keine statischen Variablen
- ❌ Keine const Variablen

### Sonstiges

- ❌ Keine Pointer
- ❌ Kein Heap-Allocation (nur Stack)
- ❌ Keine Standard-Library (außer `printf` und `putchar`)
- ❌ Keine Module/Imports

---

## Changelog

### Version 1.3 (2025-01-14) - **STABLE**

**Kritische Fixes:**
- ✅ **Klammer-Parsing-Bug behoben:** Funktionen und Schleifen werden jetzt korrekt beendet
- ✅ **Stack-Overflow-Fix:** Stack-Reserve auf 2048 Bytes erhöht
- ✅ **Variable Scope:** Korrekte Cleanup nach Schleifen und Funktionen

**Verbesserungen:**
- Robusteres Parsing von geschachtelten Strukturen
- Bessere Error-Messages beim Klammer-Mismatch

### Version 1.2 (2025-01-14)

**Neue Features:**
- ✅ Klammer-Unterstützung in Expressions: `(a + b) * c`
- ✅ Verschachtelte Funktionsaufrufe: `add(10, multiply(2, 3))`
- ✅ Klammer-bewusstes Argument-Parsing

**Fixes:**
- Windows x64 Calling Convention korrekt implementiert (32-Byte Shadow Space)
- Stack-Alignment-Probleme behoben
- Buffer-Größen erhöht (500KB Code-Buffer, 1000 String-Literals)

**Limits erhöht:**
- `MAX_VARS: 100 → 200`
- `MAX_LINE: 256 → 512`
- `MAX_TOKEN: 64 → 128`
- `MAX_FUNCTIONS: 50 → 100`

### Version 1.1 (2025-01-13)

**Neue Features:**
- Loop-Scope für For-Schleifen
- Inkrement/Dekrement-Operatoren (`i++`, `i--`)
- Compound Assignment (`+=`, `-=`, `*=`, `/=`)
- Obligatorische `main()` Funktion
- Vergleichsoperatoren (`<`, `<=`, `>`, `>=`, `==`, `!=`)

### Version 1.0 (2025-01-12)

**Initiale Features:**
- Funktionen mit Parametern
- Variablen (int)
- Arithmetische Operatoren
- For-Schleifen
- Print-Statement mit String-Konkatenation
- Code-Generation für Windows x64

---

## FAQ

### Warum nur int-Typ?

Der Compiler ist ein **Lernprojekt** und fokussiert sich auf die Grundlagen der Compiler-Entwicklung. Die Erweiterung um weitere Typen ist geplant.

### Warum keine if-Statements?

Die Sprache zeigt, dass man auch **ohne klassische if-Statements** komplexe Logik implementieren kann (z.B. mit For-Schleifen als Conditionals). Eine echte `if`-Implementation ist für die nächste Version geplant.

### Wie kann ich Rekursion implementieren?

Aktuell ist **keine echte Rekursion** möglich, da der Stack zu schnell überläuft. Iterative Lösungen sind der empfohlene Weg.

### Funktioniert der Compiler auf Linux/Mac?

Der Compiler generiert **Windows x64 Assembly** mit der Windows Calling Convention. Für Linux/Mac müsste die Calling Convention (SystemV ABI) und die Assembly-Syntax angepasst werden.

### Kann ich den Compiler erweitern?

**Ja!** Der Code ist Open Source und kann frei erweitert werden. Mögliche Erweiterungen:

- Strings als Datentyp
- Arrays
- if/else-Statements
- while-Schleifen
- Mehr Operatoren (`%`, `&`, `|`, `^`, `<<`, `>>`)
- Floats/Doubles
- Structs
- Function Pointers

---

## Nutzung

### Kommandozeilenoptionen

```bash
# Standard-Kompilierung
./compiler.exe <source_file>

# Mit Debug-Ausgabe
./compiler.exe --debug <source_file>

# Debug in Datei umleiten
./compiler.exe --debug <source_file> 2> debug.log
```

### Kompilierungs-Workflow

```bash
# Schritt 1: Source zu Assembly
./compiler.exe program.txt
# Ausgabe: program.txt.s

# Schritt 2: Assembly zu Binary
gcc program.txt.s -o program.exe

# Schritt 3: Ausführen
./program.exe
```

### Ein-Schritt-Kompilierung (mit Script)

Erstelle `compile.bat`:

```batch
@echo off
compiler.exe %1
if errorlevel 1 goto error
gcc %1.s -o %~n1.exe
if errorlevel 1 goto error
%~n1.exe
goto end

:error
echo Kompilierung fehlgeschlagen!

:end
```

Dann:

```bash
compile.bat program.txt
```

---

## Technische Details

### Assembly-Ausgabe-Beispiel

Für diesen Code:

```javascript
func add(a:int, b:int) -> int
{
  return a + b;
}
```

Wird folgende Assembly generiert:

```asm
.globl add
add:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp
    movl %ecx, -4(%rbp)      ; Speichere Parameter a
    movl %edx, -8(%rbp)      ; Speichere Parameter b
    movl -4(%rbp), %eax      ; Lade a
    pushq %rax
    movl -8(%rbp), %eax      ; Lade b
    pushq %rax
    popq %rbx
    popq %rax
    addl %ebx, %eax          ; a + b
    pushq %rax
    popq %rax                ; Return-Wert in RAX
    leave
    ret
```

### Operator-Precedence-Implementierung

Der Compiler nutzt **Operator Precedence Climbing** mit rekursivem Descent:

1. Niedrigste Priorität: Vergleiche (`<`, `>`, etc.)
2. Mittlere Priorität: Addition/Subtraktion (`+`, `-`)
3. Höchste Priorität: Multiplikation/Division (`*`, `/`)
4. Höchste Priorität: Klammern `()`, Funktionsaufrufe

**Beispiel:** `a + b * c` wird geparst als `a + (b * c)`

---

## Lizenz

Dieses Projekt ist **Public Domain**. Du kannst den Code frei nutzen, modifizieren und verteilen.

---

## Kontakt

**Autor:** Philipp01105  
**GitHub:** (Füge deine GitHub-URL hier ein)  
**Datum:** 2025-01-14

---

## Danksagung

Danke an alle, die beim Debuggen und Testen geholfen haben! 🎉

---

**Viel Spaß beim Programmieren!** 🚀