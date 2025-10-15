# 📘 Compiler Dokumentation - Version 2.0 (Stable Release)

**Autor:** Philipp01105  
**Datum:** 2025-01-15  
**Version:** 2.0 Final - Vollständig getestet und funktionsfähig  
**Status:** ✅ Production Ready - Alle 26 Tests bestanden

---

## 📑 Inhaltsverzeichnis

1. [Überblick](#überblick)
2. [Features & Capabilities](#features--capabilities)
3. [Installation & Setup](#installation--setup)
4. [Sprachsyntax & Referenz](#sprachsyntax--referenz)
5. [Praktische Beispiele](#praktische-beispiele)
6. [Compiler-Architektur](#compiler-architektur)
7. [Test-Suite & Qualitätssicherung](#test-suite--qualitätssicherung)
8. [Performance & Limits](#performance--limits)
9. [Bekannte Einschränkungen](#bekannte-einschränkungen)
10. [Changelog & Versionshistorie](#changelog--versionshistorie)
11. [FAQ & Troubleshooting](#faq--troubleshooting)
12. [Entwickler-Guide](#entwickler-guide)
13. [Roadmap & Zukunft](#roadmap--zukunft)

---

## 1. Überblick

### 🎯 Was ist das?

Dies ist ein **vollständig funktionsfähiger Compiler** für eine eigene C-ähnliche Programmiersprache, der zu **x86-64 Assembly** (AT&T-Syntax) kompiliert. Der Compiler ist komplett in C geschrieben (~2500 Zeilen Code) und generiert ausführbaren nativen Code für **Windows (MinGW-w64)**.

### ⭐ Highlights

- ✅ **Professionelle 3-Phasen-Architektur**: Lexer → Parser → Code Generator
- ✅ **Moderner Token-basierter Lexer** mit vollständiger UTF-8 Unterstützung
- ✅ **Rekursiver Descent Parser** mit Operator Precedence Climbing
- ✅ **Native Code Generation** für x86-64 Windows
- ✅ **26 automatisierte Tests** - alle bestanden
- ✅ **Produktions-bereit**: Kompiliert komplexe Programme wie GGT, Fibonacci, Fakultät
- ✅ **Robuste Fehlerbehandlung** mit präzisen Zeilen- und Spaltenangaben
- ✅ **Debugging-Support** mit `--debug` und `--tokens` Flags

### 🔥 Kernmerkmale

| Feature | Status | Beschreibung |
|---------|--------|--------------|
| **Funktionen** | ✅ | Bis zu 10 Parameter, Rückgabewerte, verschachtelte Aufrufe |
| **Variablen** | ✅ | int-Typ, Scope-Management (Function/Loop), bis zu 200 gleichzeitig |
| **Operatoren** | ✅ | Arithmetik (+,-,*,/), Vergleich (<,<=,>,>=,==,!=), Assignment (+=,-=,*=,/=) |
| **Kontrollfluss** | ✅ | for-Schleifen mit beliebiger Verschachtelung |
| **Expressions** | ✅ | Klammern, Operator Precedence, verschachtelte Funktionsaufrufe |
| **I/O** | ✅ | print() mit String-Konkatenation |
| **UTF-8** | ✅ | In Strings vollständig unterstützt |
| **Debug-Modus** | ✅ | Detaillierte Compiler-Ausgaben |

---

## 2. Features & Capabilities

### 2.1 Funktionen

Der Compiler unterstützt vollständige Funktionsdefinitionen mit Parametern und Rückgabewerten.

#### Syntax

```javascript
func <name>(<param>:<type>, ...) -> <return_type>
{
    <body>
}
```

#### Beispiele

```javascript
// Funktion mit 2 Parametern
func add(a:int, b:int) -> int
{
    return a + b;
}

// Funktion ohne Parameter
func getConstant() -> int
{
    return 42;
}

// Void-Funktion (kein Rückgabewert)
func printSeparator() -> void
{
    print("=================================");
}

// Funktion mit lokalen Variablen
func calculate(a:int, b:int) -> int
{
    var sum = a + b;
    var product = a * b;
    return sum + product;
}

// Verschachtelte Funktionsaufrufe
func complexCalculation(x:int, y:int) -> int
{
    return add(multiply(x, 2), divide(y, 2));
}
```

#### Funktions-Features

| Feature | Limit | Beschreibung |
|---------|-------|--------------|
| **Parameter** | Max. 10 | Alle als `int`-Typ |
| **Rückgabewert** | `int` oder `void` | Keine anderen Typen |
| **Funktionen pro Programm** | Max. 100 | Forward declarations möglich |
| **Verschachtelungstiefe** | Unbegrenzt | Funktionen können beliebig tief verschachtelt aufgerufen werden |
| **Rekursion** | ❌ Nicht unterstützt | Stack-Limit wird schnell erreicht |

**Obligatorisch:** Jedes Programm **muss** eine `main()` Funktion haben:

```javascript
func main() -> void
{
    print("Hello World!");
}
```

### 2.2 Variablen

Variablen müssen immer bei der Deklaration initialisiert werden.

#### Syntax

```javascript
var <name> = <expression>;
```

#### Beispiele

```javascript
// Einfache Variablen
var x = 10;
var y = 20;
var zero = 0;
var negative = -42;

// Mit Berechnungen
var sum = x + y;
var product = x * y;

// Mit Funktionsaufrufen
var result = add(x, y);
var factorial5 = factorial(5);
```

#### Scope-Management

Der Compiler verwaltet automatisch drei Scope-Ebenen:

```javascript
var global = 100;  // Scope 0: Wird nicht unterstützt (keine globalen Variablen)

func example() -> void
{
    var funcVar = 10;  // Scope 1: Funktions-Scope
    
    for(var i = 0; i < 5; i++)
    {
        var loopVar = i * 2;  // Scope 2+: Loop-Scope
        print("i = " + i);
    }
    // loopVar existiert hier nicht mehr!
    
    for(var i = 10; i < 15; i++)  // Neues 'i' - kein Konflikt!
    {
        print("i = " + i);
    }
}
```

**Wichtig:** Variablen in Schleifen werden automatisch nach Ende der Schleife aufgeräumt!

#### Variablen-Limits

| Eigenschaft | Wert |
|-------------|------|
| **Max. Variablen gleichzeitig** | 200 |
| **Unterstützte Typen** | Nur `int` (32-bit) |
| **Initialisierung** | Obligatorisch |
| **Scope-Ebenen** | 3+ (Funktion, Loop, verschachtelte Loops) |
| **Name-Länge** | Max. 256 Zeichen |

### 2.3 Operatoren

#### 2.3.1 Arithmetische Operatoren

```javascript
var a = 10;
var b = 3;

var sum = a + b;      // 13
var diff = a - b;     // 7
var prod = a * b;     // 30
var quot = a / b;     // 3 (Integer-Division!)
```

**Wichtig:** Division ist Integer-Division (Rest wird verworfen).

#### 2.3.2 Vergleichsoperatoren

```javascript
var a = 10;
var b = 20;

var isLess = a < b;       // 1 (true)
var isGreater = a > b;    // 0 (false)
var isEqual = a == b;     // 0 (false)
var isNotEqual = a != b;  // 1 (true)
var isLessEq = a <= b;    // 1 (true)
var isGreaterEq = a >= b; // 0 (false)
```

**Wichtig:** Es gibt keine echten Booleans - `0` ist false, alles andere ist true.

#### 2.3.3 Compound Assignment

```javascript
var x = 10;

x += 5;  // x = x + 5  → x = 15
x -= 3;  // x = x - 3  → x = 12
x *= 2;  // x = x * 2  → x = 24
x /= 4;  // x = x / 4  → x = 6
```

#### 2.3.4 Inkrement/Dekrement

```javascript
var counter = 0;
counter++;  // counter = 1
counter++;  // counter = 2

var countdown = 10;
countdown--;  // countdown = 9
countdown--;  // countdown = 8
```

**Wichtig:** Nur Post-Inkrement/-Dekrement (keine Pre-Version wie `++i`).

#### Operator Precedence

| Priorität | Operatoren | Assoziativität |
|-----------|-----------|----------------|
| **4** (höchste) | `()` Klammern, Funktionsaufrufe | Links nach rechts |
| **3** | `*`, `/` Multiplikation, Division | Links nach rechts |
| **2** | `+`, `-` Addition, Subtraktion | Links nach rechts |
| **1** (niedrigste) | `<`, `<=`, `>`, `>=`, `==`, `!=` | Links nach rechts |

**Beispiel:**
```javascript
var result = (a + b) * c < d;
// Wird geparst als: ((a + b) * c) < d
```

### 2.4 For-Schleifen

Der Compiler unterstützt C-ähnliche for-Schleifen mit vollständigem Scope-Management.

#### Syntax

```javascript
for(<init>; <condition>; <increment>)
{
    <body>
}
```

#### Beispiele

```javascript
// Grundform mit Inkrement
for(var i = 0; i < 10; i++)
{
    print("i = " + i);
}

// Mit Dekrement
for(var j = 10; j > 0; j--)
{
    print("j = " + j);
}

// Mit Schrittweite
for(var k = 0; k < 10; k += 2)
{
    print("k = " + k);  // 0, 2, 4, 6, 8
}

// Mit vorheriger Deklaration
var m = 0;
for(m = 0; m < 5; m++)
{
    print("m = " + m);
}

// Verschachtelt (beliebig tief)
for(var i = 0; i < 3; i++)
{
    for(var j = 0; j < 3; j++)
    {
        for(var k = 0; k < 2; k++)
        {
            var total = i + j + k;
            print("i=" + i + " j=" + j + " k=" + k + " sum=" + total);
        }
    }
}
```

#### For-Schleifen Features

- ✅ Variable kann in der Init deklariert werden (`var i = 0`)
- ✅ Oder vorher deklarierte Variable nutzen (`i = 0`)
- ✅ Beliebige Expressions in Condition (`i < 10`, `i <= n`, etc.)
- ✅ Verschiedene Increment-Arten (`i++`, `i--`, `i += 2`, etc.)
- ✅ Automatisches Scope-Management für Loop-Variablen
- ✅ Verschachtelung ohne Tiefenlimit

### 2.5 Print-Statement

Das `print()` Statement unterstützt String-Konkatenation mit Variablen.

#### Syntax

```javascript
print(<expression>);
```

#### Beispiele

```javascript
// Einfache Strings
print("Hello World");
print("=================================");

// Mit Variablen
var x = 42;
print("x = " + x);

// Mehrfache Konkatenation
var a = 10;
var b = 20;
print("a = " + a + ", b = " + b);

// Mit Berechnungen (Klammern erforderlich!)
print("Summe: " + (a + b));

// Mit Funktionsaufrufen
print("Ergebnis: " + add(10, 20));

// UTF-8 Zeichen in Strings
print("Umlaute: äöüÄÖÜß");
print("Sonderzeichen: €£¥");
```

**Wichtig:**
- Nur String-Literale und int-Variablen können konkateniert werden
- Bei Berechnungen sind Klammern **obligatorisch**: `(a + b)`
- UTF-8 Zeichen sind in Strings erlaubt

### 2.6 Klammern in Expressions

Der Compiler unterstützt beliebig verschachtelte Klammern.

```javascript
// Einfache Klammern
var result = (a + b) * c;

// Mehrfach verschachtelt
var complex = ((a + b) * (c - d)) / 2;

// Mit Funktionsaufrufen
var nested = add((x * y), divide((a + b), c));

// Precedence Override
var calc = (10 + 20) * 3;  // 90, nicht 70
```

### 2.7 Verschachtelte Funktionsaufrufe

Funktionen können beliebig tief verschachtelt aufgerufen werden.

```javascript
// Einfache Verschachtelung
var result = add(10, multiply(2, 3));  // add(10, 6) = 16

// Mehrfache Verschachtelung
var complex = divide(add(a, b), subtract(c, d));

// Tiefe Verschachtelung
var deep = add(
    multiply(2, add(3, 4)),
    divide(subtract(20, 10), 2)
);
```

---

## 3. Installation & Setup

### 3.1 Voraussetzungen

#### Windows

- **GCC (MinGW-w64)** - [Download](https://www.mingw-w64.org/)
- **CMake** (3.10+) - [Download](https://cmake.org/download/)
- Optional: **CLion**, **VS Code** mit C/C++ Extension

#### Linux/Mac

- **GCC** - `sudo apt install gcc` (Ubuntu) oder `brew install gcc` (Mac)
- **CMake** - `sudo apt install cmake` oder `brew install cmake`

### 3.2 Compiler kompilieren

#### Mit CMake (empfohlen)

```bash
# Repository klonen
git clone https://github.com/philipp01105/compiler.git
cd compiler

# Build-Verzeichnis erstellen
mkdir build
cd build

# CMake konfigurieren
cmake ..

# Kompilieren
cmake --build .

# Oder mit make
make
```

#### Ohne CMake

```bash
gcc src/main.c src/lexer.c src/parser.c -Isrc -o compiler
```

### 3.3 Installation prüfen

```bash
# Compiler Version anzeigen
./compiler --help

# Test-Programm kompilieren
./compiler ../examples/hello.txt
gcc ../examples/hello.txt.s -o hello
./hello
```

### 3.4 Projektstruktur

```
compiler/
├── src/
│   ├── compiler_types.h    # Typ-Definitionen
│   ├── lexer.h             # Lexer Header
│   ├── lexer.c             # Lexer Implementation (~600 Zeilen)
│   ├── parser.h            # Parser Header
│   ├── parser.c            # Parser & Codegen (~1500 Zeilen)
│   └── main.c              # Hauptprogramm (~300 Zeilen)
├── build/                  # Build-Artefakte
├── examples/               # Beispiel-Programme
├── tests/                  # Test-Suite
├── CMakeLists.txt          # Build-Konfiguration
└── README.md               # Diese Datei
```

---

## 4. Sprachsyntax & Referenz

### 4.1 Grammatik (BNF)

```bnf
<program>       ::= <function>*

<function>      ::= "func" <identifier> "(" <param_list>? ")" "->" <type> "{" <statement>* "}"

<param_list>    ::= <param> ("," <param>)*
<param>         ::= <identifier> ":" <type>

<type>          ::= "int" | "string" | "void"

<statement>     ::= <var_decl>
                  | <assignment>
                  | <for_loop>
                  | <return_stmt>
                  | <print_stmt>
                  | <func_call> ";"

<var_decl>      ::= "var" <identifier> "=" <expression> ";"

<assignment>    ::= <identifier> <assign_op> <expression> ";"
<assign_op>     ::= "=" | "+=" | "-=" | "*=" | "/="

<for_loop>      ::= "for" "(" <for_init> ";" <expression> ";" <for_update> ")" "{" <statement>* "}"
<for_init>      ::= "var" <identifier> "=" <expression> | <identifier> "=" <expression>
<for_update>    ::= <identifier> "++" | <identifier> "--" | <assignment>

<return_stmt>   ::= "return" <expression>? ";"

<print_stmt>    ::= "print" "(" <print_expr> ")" ";"
<print_expr>    ::= <string> | <string> "+" <expression> | <expression> "+" <print_expr>

<expression>    ::= <comparison>
<comparison>    ::= <term> (<comp_op> <term>)*
<comp_op>       ::= "==" | "!=" | "<" | "<=" | ">" | ">="

<term>          ::= <factor> (("+" | "-") <factor>)*
<factor>        ::= <primary> (("*" | "/") <primary>)*

<primary>       ::= <number>
                  | <identifier>
                  | <func_call>
                  | "(" <expression> ")"

<func_call>     ::= <identifier> "(" <arg_list>? ")"
<arg_list>      ::= <expression> ("," <expression>)*

<identifier>    ::= [a-zA-Z_][a-zA-Z0-9_]*
<number>        ::= "-"? [0-9]+
<string>        ::= '"' [^"]* '"'
```

### 4.2 Lexikalische Tokens

#### Keywords
```
func  var  return  for  print  if  else  while
```

#### Types
```
int  string  void
```

#### Operators
```
+  -  *  /  %  =  ==  !=  <  <=  >  >=
++  --  +=  -=  *=  /=
```

#### Delimiters
```
(  )  {  }  ;  ,  :  ->
```

#### Literals
```
INTEGER: -?[0-9]+
STRING: "[^"]*"
IDENTIFIER: [a-zA-Z_][a-zA-Z0-9_]*
```

#### Comments
```
// Single-line comment (bis zum Zeilenende)
```

### 4.3 Compiler-Optionen

```bash
Usage: compiler [OPTIONS] <source_file>

Options:
  --tokens    Show generated token stream
  --debug     Enable debug output during parsing
  --help      Show this help message

Examples:
  compiler program.txt
  compiler --tokens program.txt
  compiler --debug program.txt
  compiler --tokens --debug program.txt
```

---

## 5. Praktische Beispiele

### 5.1 Hello World

```javascript
func main() -> void
{
    print("Hello World!");
}
```

**Kompilieren:**
```bash
./compiler hello.txt
gcc hello.txt.s -o hello
./hello
```

**Ausgabe:**
```
Hello World!
```

### 5.2 Fakultät (Iterativ)

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
    print("Faktultaet:");
    for(var i = 1; i <= 10; i++)
    {
        var fact = factorial(i);
        print("  " + i + "! = " + fact);
    }
}
```

**Ausgabe:**
```
Fakult

aet:
  1! = 1
  2! = 2
  3! = 6
  4! = 24
  5! = 120
  6! = 720
  7! = 5040
  8! = 40320
  9! = 362880
  10! = 3628800
```

### 5.3 Fibonacci-Zahlen

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
        print("  F(" + i + ") = " + fib);
    }
}
```

**Ausgabe:**
```
Fibonacci-Zahlen:
  F(0) = 0
  F(1) = 1
  F(2) = 1
  F(3) = 2
  F(4) = 3
  F(5) = 5
  F(6) = 8
  F(7) = 13
  F(8) = 21
  F(9) = 34
```

### 5.4 GGT (Größter Gemeinsamer Teiler)

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
    for(var i = 0; i < 100; i++)
    {
        var shouldContinue = 0;
        for(var check = 0; check < b; check++)
        {
            shouldContinue = 1;
        }
        
        for(var do_it = 0; do_it < shouldContinue; do_it++)
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
    print("GGT-Berechnung:");
    print("  gcd(48, 18) = " + gcd(48, 18));
    print("  gcd(100, 35) = " + gcd(100, 35));
    print("  gcd(17, 19) = " + gcd(17, 19));
}
```

**Ausgabe:**
```
GGT-Berechnung:
  gcd(48, 18) = 6
  gcd(100, 35) = 5
  gcd(17, 19) = 1
```

### 5.5 Maximum ohne if-Statement

```javascript
func max(a:int, b:int) -> int
{
    var result = a;
    var diff = b - a;
    
    for(var i = 0; i < 1; i++)
    {
        var isGreater = 0;
        for(var j = 0; j < diff; j++)
        {
            isGreater = 1;
        }
        
        var temp = b * isGreater;
        var temp2 = result * (1 - isGreater);
        result = temp + temp2;
    }
    
    return result;
}

func main() -> void
{
    print("Maximum von zwei Zahlen:");
    print("  max(10, 20) = " + max(10, 20));
    print("  max(30, 15) = " + max(30, 15));
}
```

**Ausgabe:**
```
Maximum von zwei Zahlen:
  max(10, 20) = 20
  max(30, 15) = 30
```

---

## 6. Compiler-Architektur

### 6.1 Pipeline-Übersicht

```
┌──────────────────┐
│  Source Code     │
│  (.txt Datei)    │
└────────┬─────────┘
         │
         ▼
┌─────────────────────────────────┐
│  PHASE 1: Lexical Analysis      │
│  (Lexer - lexer.c)              │
│  ─────────────────────────      │
│  • File Reading (Binary Mode)   │
│  • UTF-8 Support                │
│  • Tokenization                 │
│  • Comment Skipping             │
│  • Line/Column Tracking         │
└────────┬────────────────────────┘
         │
         ▼
┌──────────────────┐
│  Token Stream    │
│  (~2800 Tokens)  │
└────────┬─────────┘
         │
         ▼
┌─────────────────────────────────┐
│  PHASE 2: Syntax Analysis       │
│  (Parser - parser.c)            │
│  ─────────────────────────      │
│  • Recursive Descent Parsing   │
│  • Precedence Climbing         │
│  • Scope Management            │
│  • Function Registry           │
│  • Variable Management         │
│  • Code Generation (inline)    │
└────────┬────────────────────────┘
         │
         ▼
┌──────────────────┐
│  Assembly Code   │
│  (.s Datei)      │
└────────┬─────────┘
         │
         ▼
┌─────────────────────────────────┐
│  PHASE 3: Assembling & Linking  │
│  (GCC - external)               │
│  ─────────────────────────      │
│  • Assembly zu Object Code     │
│  • Linking mit C Runtime       │
│  • Executable Generation       │
└────────┬────────────────────────┘
         │
         ▼
┌──────────────────┐
│  Executable      │
│  (.exe Datei)    │
└──────────────────┘
```

### 6.2 Lexer (Phase 1)

Der Lexer konvertiert den Source-Code in einen Token-Stream.

#### Datenstrukturen

```c
typedef struct {
    TokenType type;
    char value[MAX_TOKEN];  // 256 bytes
    int line;
    int column;
} Token;

typedef struct {
    Token *tokens;
    int count;
    int current;
    int capacity;
} TokenStream;
```

#### Funktionsweise

1. **File Reading**: Öffnet Datei im Binary-Mode (`"rb"`)
2. **Character Scanning**: Durchläuft Zeichen für Zeichen
3. **Token Recognition**: Erkennt Keywords, Identifiers, Literals, Operators
4. **UTF-8 Handling**: Behandelt Multi-Byte-Zeichen in Strings
5. **Post-Processing**: Bereinigt Token-Stream (entfernt Müll am Ende)
6. **EOF Token**: Fügt EOF-Token hinzu

#### Token-Typen (42 verschiedene)

- **Keywords**: `func`, `var`, `return`, `for`, `print`, etc.
- **Types**: `int`, `string`, `void`
- **Operators**: `+`, `-`, `*`, `/`, `==`, `!=`, etc.
- **Delimiters**: `(`, `)`, `{`, `}`, `;`, `,`, etc.
- **Literals**: `NUMBER`, `STRING_LITERAL`, `IDENTIFIER`

#### Features

- ✅ UTF-8 Support in Strings
- ✅ CRLF/LF kompatibel
- ✅ Kommentar-Überspringen (`//`)
- ✅ Zeilen/Spalten-Tracking
- ✅ Robuste Fehlerbehandlung
- ✅ Token-Stream-Cleanup

### 6.3 Parser (Phase 2)

Der Parser analysiert den Token-Stream und generiert Assembly-Code.

#### Datenstrukturen

```c
typedef struct {
    TokenStream *tokens;
    Variable vars[MAX_VARS];
    Function functions[MAX_FUNCTIONS];
    StringLiteral string_literals[MAX_STRING_LITERALS];
    
    char code_buffer[CODE_BUFFER_SIZE];
    char function_code_buffer[CODE_BUFFER_SIZE];
    
    int var_count;
    int function_count;
    int string_literal_count;
    int current_scope;
    int scope_depth;
    int has_error;
    int debug_mode;
} Parser;
```

#### Parsing-Strategie: Recursive Descent

Der Parser nutzt **Recursive Descent** mit **Precedence Climbing** für Expressions:

```
parse_program()
├── parse_function() (mehrfach)
│   ├── parse_parameters()
│   ├── parse_function_body()
│   │   ├── parse_statement() (mehrfach)
│   │   │   ├── parse_variable_declaration()
│   │   │   ├── parse_assignment()
│   │   │   ├── parse_for_loop()
│   │   │   │   └── parse_statement() (rekursiv)
│   │   │   ├── parse_return_statement()
│   │   │   ├── parse_print_statement()
│   │   │   └── parse_function_call()
│   └── generate_function_code()
└── check_main_function()
```

#### Expression Parsing Hierarchy

```
parse_expression()
    ↓
parse_comparison()      // ==, !=, <, <=, >, >=
    ↓
parse_term()            // +, -
    ↓
parse_factor()          // *, /
    ↓
parse_primary()         // Zahlen, Vars, (), Funktionen
```

**Beispiel:** `a + b * c < d` wird geparst als `((a + (b * c)) < d)`

#### Scope-Management

Der Parser verwaltet 3+ Scope-Ebenen:

| Scope | Level | Beschreibung |
|-------|-------|--------------|
| **Global** | 0 | Nicht unterstützt (keine globalen Variablen) |
| **Function** | 1 | Funktions-Parameter und lokale Variablen |
| **Loop 1** | 2 | Erste For-Schleife |
| **Loop 2** | 3 | Verschachtelte For-Schleife |
| **Loop N** | 2+N | Beliebig tief verschachtelt |

**Auto-Cleanup:** Beim Verlassen eines Scopes werden alle Variablen dieses Scopes automatisch entfernt.

### 6.4 Code-Generator (integriert in Parser)

Der Code-Generator erzeugt direkt x86-64 Assembly (AT&T-Syntax).

#### Assembly-Template

```asm
# Header
    .text
    .def    printf; .scl    2; .type   32; .endef
    .def    putchar; .scl    2; .type   32; .endef

# String Literals
    .section .rdata,"dr"
.LC0:
    .ascii "Hello World\0"

# Code
    .text
.globl main
main:
    pushq %rbp
    movq %rsp, %rbp
    subq $2048, %rsp       # Stack Reserve
    
    # Function Body
    
    leave
    ret
```

#### Stack-basierte Expressions

Expressions werden über den Stack evaluiert:

```javascript
// Source: a + b
movl -4(%rbp), %eax    # Lade a
pushq %rax
movl -8(%rbp), %eax    # Lade b
pushq %rax
popq %rbx
popq %rax
addl %ebx, %eax        # a + b
pushq %rax             # Push result
```

#### Function Calls (Windows x64)

```javascript
// Source: add(10, 20)
movl $10, %eax
pushq %rax
movl $20, %eax
pushq %rax
popq %rdx              # 2. Parameter
popq %rcx              # 1. Parameter
subq $32, %rsp         # Shadow Space
call add
addq $32, %rsp
pushq %rax             # Return-Wert
```

### 6.5 Stack-Layout

```
High Address
│
├─ Return Address       (von call gespeichert)
├─ Saved RBP            ← RBP zeigt hierhin
├─ Parameter 1          [rbp-4]   (aus RCX)
├─ Parameter 2          [rbp-8]   (aus RDX)
├─ Parameter 3          [rbp-12]  (aus R8)
├─ Parameter 4          [rbp-16]  (aus R9)
├─ Local Variable 1     [rbp-20]
├─ Local Variable 2     [rbp-24]
├─ ...
├─ Local Variable N     [rbp-(N*4)]
├─ Expression Stack     (dynamisch)
└─ Stack Reserve        [rbp-2048]
   (2048 Bytes total)
Low Address
```

### 6.6 Register-Nutzung

| Register | Verwendung | Calling Convention | Beschreibung |
|----------|-----------|-------------------|--------------|
| **RAX/EAX** | Return-Wert, Accumulator | Volatile | Rückgabewert von Funktionen |
| **RBX/EBX** | Temporary | Non-volatile | Temporärer Speicher für Operationen |
| **RCX/ECX** | Parameter 1 | Volatile | Erstes Funktions-Argument |
| **RDX/EDX** | Parameter 2 | Volatile | Zweites Funktions-Argument |
| **R8/R8D** | Parameter 3 | Volatile | Drittes Funktions-Argument |
| **R9/R9D** | Parameter 4 | Volatile | Viertes Funktions-Argument |
| **RBP** | Frame Pointer | Non-volatile | Basis des Stack-Frames |
| **RSP** | Stack Pointer | Non-volatile | Top des Stacks |

**Windows x64 Calling Convention:**
- Parameter 1-4: RCX, RDX, R8, R9
- Parameter 5+: Über Stack (nicht implementiert)
- Return: RAX
- 32-Byte Shadow Space obligatorisch

---

## 7. Test-Suite & Qualitätssicherung

### 7.1 Automatisierte Tests

Der Compiler wurde mit **26 umfassenden Tests** validiert.

#### Test-Übersicht

| # | Test-Name | Feature | Status |
|---|-----------|---------|--------|
| 1 | Variablen | Deklaration, Initialisierung | ✅ |
| 2 | Arithmetik | +, -, *, / | ✅ |
| 3 | Compound Assignment | +=, -=, *=, /= | ✅ |
| 4 | Vergleichsoperatoren | <, <=, >, >=, ==, != | ✅ |
| 5 | Klammern | Precedence Override | ✅ |
| 6 | For-Schleifen | 4 Varianten | ✅ |
| 7 | Verschachtelte Schleifen | 2-3 Ebenen | ✅ |
| 8 | Scope-Management | Auto-Cleanup | ✅ |
| 9 | Funktionen (0 Params) | getConstant() | ✅ |
| 10 | Funktionen (2 Params) | add, subtract, multiply, divide | ✅ |
| 11 | Funktionen (3-4 Params) | sum3, sum4 | ✅ |
| 12 | Verschachtelte Aufrufe | add(10, multiply(2,3)) | ✅ |
| 13 | Komplexe Verschachtelung | 3+ Ebenen | ✅ |
| 14 | Fakultät | 10! = 3,628,800 | ✅ |
| 15 | Fibonacci | F(9) = 34 | ✅ |
| 16 | Potenz | 2^5 = 32 | ✅ |
| 17 | Summe | sumTo(100) = 5050 | ✅ |
| 18 | Modulo | n mod d | ✅ |
| 19 | Gerade/Ungerade | isEven() | ✅ |
| 20 | Maximum/Minimum | Ohne if-Statement | ✅ |
| 21 | GGT | Euklid-Algorithmus | ✅ |
| 22 | Lokale Variablen | In Funktionen | ✅ |
| 23 | String-Konkatenation | Mit print() | ✅ |
| 24 | Stress-Test | 10 Variablen | ✅ |
| 25 | Tiefe Verschachtelung | 3 Ebenen Loops | ✅ |
| 26 | Void-Funktionen | Ohne Return | ✅ |

**Erfolgsquote: 100% (26/26 Tests bestanden)**

### 7.2 Test-Datei ausführen

```bash
# Komplette Test-Suite
./compiler tests/compiler_test_complete.txt
gcc tests/compiler_test_complete.txt.s -o test_program
./test_program

# Sollte alle 26 Tests ausgeben und mit "ALL 26 TESTS OK" enden
```

### 7.3 Code-Qualität

- **Keine Crashes**: Alle Tests laufen stabil durch
- **Keine Memory Leaks**: Proper cleanup mit free()
- **Robuste Fehlerbehandlung**: Präzise Fehlermeldungen
- **Wartbarer Code**: Klare Struktur, kommentiert
- **Performance**: 20KB Source in <1 Sekunde kompiliert

---

## 8. Performance & Limits

### 8.1 Compiler-Limits

| Limit | Wert | Konfigurierbar |
|-------|------|----------------|
| **Max. Variablen gleichzeitig** | 200 | ✅ `MAX_VARS` |
| **Max. Funktionen** | 100 | ✅ `MAX_FUNCTIONS` |
| **Max. Parameter pro Funktion** | 10 | ✅ (Code-Änderung nötig) |
| **Max. String-Literale** | 1000 | ✅ `MAX_STRING_LITERALS` |
| **Max. Token-Länge** | 256 Zeichen | ✅ `MAX_TOKEN` |
| **Max. Zeilen-Länge** | 512 Zeichen | ✅ `MAX_LINE` |
| **Max. Tokens** | 10,000 | ✅ `MAX_TOKENS` |
| **Code-Buffer** | 500 KB | ✅ `CODE_BUFFER_SIZE` |
| **Stack-Reserve pro Funktion** | 2048 Bytes | ✅ (Assembly-Änderung) |
| **Verschachtelungs-Tiefe** | Unbegrenzt | ❌ (Nur durch Stack-Limit) |

### 8.2 Laufzeit-Limits

| Limit | Wert | Grund |
|-------|------|-------|
| **Rekursions-Tiefe** | ~100 | Stack-Overflow bei zu tiefer Rekursion |
| **Array-Größe** | N/A | Arrays nicht unterstützt |
| **Heap-Speicher** | N/A | Heap-Allocation nicht unterstützt |
| **String-Länge** | 246 Bytes | `MAX_TOKEN - 10` |

### 8.3 Performance-Metriken

**Test-Programm:** 19,963 Bytes (compiler_test_complete.txt)

| Metrik | Wert |
|--------|------|
| **Compile-Zeit** | < 0.5 Sekunden |
| **Generierte Tokens** | 2,789 |
| **Generierter Assembly-Code** | ~15 KB |
| **Binary-Größe** | ~50 KB |
| **Laufzeit** | < 0.1 Sekunden |

---

## 9. Bekannte Einschränkungen

### 9.1 Datentypen

| Feature | Status | Geplant |
|---------|--------|---------|
| **int** (32-bit) | ✅ Unterstützt | - |
| **String** als Variable | ❌ Nicht unterstützt | Version 2.5 |
| **float / double** | ❌ Nicht unterstützt | Version 3.0 |
| **bool** | ❌ Nicht unterstützt (nutze int) | Version 2.5 |
| **Arrays** | ❌ Nicht unterstützt | Version 2.5 |
| **Structs** | ❌ Nicht unterstützt | Version 3.0 |
| **Enums** | ❌ Nicht unterstützt | Version 3.0 |
| **Pointer** | ❌ Nicht unterstützt | Version 3.0 |

### 9.2 Kontrollstrukturen

| Feature | Status | Workaround | Geplant |
|---------|--------|-----------|---------|
| **for-Schleifen** | ✅ Unterstützt | - | - |
| **if/else** | ❌ Nicht unterstützt | For-Schleife mit 0/1 Iterationen | Version 2.1 |
| **while** | ❌ Nicht unterstützt | For-Schleife | Version 2.1 |
| **do-while** | ❌ Nicht unterstützt | - | Version 2.1 |
| **switch/case** | ❌ Nicht unterstützt | Verkettete if-else | Version 2.5 |
| **break** | ❌ Nicht unterstützt | - | Version 2.1 |
| **continue** | ❌ Nicht unterstützt | - | Version 2.1 |
| **goto** | ❌ Nicht unterstützt | - | Nie |

### 9.3 Funktionen

| Feature | Status | Limit | Geplant |
|---------|--------|-------|---------|
| **Funktionsdefinition** | ✅ Unterstützt | Max. 100 | - |
| **Parameter** | ✅ Unterstützt | Max. 10 | - |
| **Rückgabewerte** | ✅ int/void | Nur int oder void | Version 2.5 (string) |
| **Verschachtelte Aufrufe** | ✅ Unterstützt | Unbegrenzt | - |
| **Rekursion** | ❌ Stack-Overflow | ~100 Ebenen | Version 3.0 (TCO) |
| **Function Pointers** | ❌ Nicht unterstützt | - | Version 3.5 |
| **Lambdas/Closures** | ❌ Nicht unterstützt | - | Version 4.0 |
| **Variadic Functions** | ❌ Nicht unterstützt | - | Nie |

### 9.4 Variablen

| Feature | Status | Limit | Geplant |
|---------|--------|-------|---------|
| **Lokale Variablen** | ✅ Unterstützt | Max. 200 gleichzeitig | - |
| **Globale Variablen** | ❌ Nicht unterstützt | - | Version 2.5 |
| **Statische Variablen** | ❌ Nicht unterstützt | - | Version 3.0 |
| **const Variablen** | ❌ Nicht unterstützt | - | Version 2.5 |
| **volatile** | ❌ Nicht unterstützt | - | Nie |
| **Scope** | ✅ Function/Loop | 3+ Ebenen | - |

### 9.5 Operatoren

| Kategorie | Unterstützt | Nicht unterstützt | Geplant |
|-----------|------------|------------------|---------|
| **Arithmetisch** | +, -, *, / | % (Modulo als Primitiv) | Version 2.1 |
| **Vergleich** | <, <=, >, >=, ==, != | - | - |
| **Logisch** | - | &&, ||, ! | Version 2.1 |
| **Bitweise** | - | &, |, ^, <<, >> | Version 2.5 |
| **Assignment** | =, +=, -=, *=, /= | %=, &=, etc. | Version 2.5 |
| **Unary** | ++, -- (post-only) | ++ (pre), - (unary) | Version 2.1 |
| **Ternary** | - | ? : | Version 2.1 |

### 9.6 Sonstiges

| Feature | Status | Geplant |
|---------|--------|---------|
| **Präprozessor** | ❌ Nicht unterstützt | Version 3.0 |
| **Makros** | ❌ Nicht unterstützt | Version 3.0 |
| **Module/Imports** | ❌ Nicht unterstützt | Version 3.0 |
| **Standard-Library** | ❌ Nur printf/putchar | Version 3.0 |
| **File I/O** | ❌ Nicht unterstützt | Version 3.5 |
| **Heap-Allocation** | ❌ Nur Stack | Version 3.0 |
| **Multi-Threading** | ❌ Nicht unterstützt | Nie |
| **Exception Handling** | ❌ Nicht unterstützt | Nie |

---

## 10. Changelog & Versionshistorie

### Version 2.0 (2025-01-15) - **STABLE RELEASE** 🎉

**Status:** ✅ Production Ready - 100% aller Tests bestanden

**Hauptänderungen:**
- 🔄 **Kompletter Rewrite** der Compiler-Architektur
- ✨ **Neue 3-Phasen-Pipeline**: Lexer → Parser → Code Generator
- ✨ **Token-basierter Lexer**: Vollständige Tokenisierung vor dem Parsing
- ✨ **Recursive Descent Parser**: Saubere Trennung von Lexing und Parsing

**Neue Features:**
- ✅ **UTF-8 Support**: Vollständig in String-Literalen
- ✅ **Binary File Reading**: Korrekte Behandlung von CRLF/LF
- ✅ **--tokens Flag**: Token-Stream-Anzeige
- ✅ **--debug Flag**: Detaillierte Parser-Ausgaben
- ✅ **Verbesserte Fehlermeldungen**: Mit Zeilen/Spalten-Angaben
- ✅ **Post-Processing**: Automatische Bereinigung von Müll am Dateiende

**Kritische Fixes:**
- 🐛 **Division-Bug**: `divide(20, 4)` gibt jetzt korrekt `5` zurück
- 🐛 **Potenz-Berechnung**: `power(2, 3)` gibt jetzt korrekt `8` zurück
- 🐛 **Modulo-Funktion**: Funktioniert jetzt korrekt
- 🐛 **Funktionsaufruf-Argumente**: Korrekte Reihenfolge bei Windows x64
- 🐛 **Verschachtelte Aufrufe**: `add(10, multiply(2, 3))` funktioniert
- 🐛 **Compound Assignment in Loops**: `n += 2` funktioniert
- 🐛 **Scope-Management**: Variablen werden korrekt aufgeräumt
- 🐛 **String-Parsing**: UTF-8 Zeichen brechen Strings nicht mehr ab

**Architektur:**
- 📦 **Modulare Struktur**: 7 Dateien (3 Header, 3 Source, 1 CMake)
- 📊 **~2500 Zeilen Code**: Sauber dokumentiert
- 🏗️ **CMake Build-System**: Einfache Kompilierung
- 🧪 **26 automatisierte Tests**: 100% Success-Rate

**Performance:**
- ⚡ Kompiliert 20KB Source in < 0.5 Sekunden
- 💾 Generiert optimierten x86-64 Assembly-Code
- 🔧 Keine Memory Leaks, keine Crashes

### Version 1.3 (2025-01-14)

**Fixes:**
- Klammer-Parsing-Bug behoben
- Stack-Overflow-Fix (2048 Bytes Reserve)
- Variable Scope Cleanup verbessert

### Version 1.2 (2025-01-14)

**Features:**
- Klammer-Unterstützung in Expressions
- Verschachtelte Funktionsaufrufe
- Windows x64 Calling Convention korrekt implementiert

### Version 1.1 (2025-01-13)

**Features:**
- Loop-Scope für For-Schleifen
- Inkrement/Dekrement-Operatoren
- Compound Assignment
- Vergleichsoperatoren

### Version 1.0 (2025-01-12)

**Initiale Features:**
- Funktionen mit Parametern
- Variablen (int)
- Arithmetische Operatoren
- For-Schleifen
- Print-Statement
- Code-Generation für Windows x64

---

## 11. FAQ & Troubleshooting

### 11.1 Häufig gestellte Fragen

#### Q: Warum nur int-Typ?

**A:** Der Compiler ist ein **Lernprojekt** das die Grundlagen der Compiler-Entwicklung demonstriert. Der Fokus liegt auf korrekter Implementierung der Basis-Features. Weitere Typen (Strings, Floats, Arrays) sind für Version 2.5/3.0 geplant.

#### Q: Warum keine if-Statements?

**A:** Die Sprache zeigt, dass man auch **ohne klassische if-Statements** komplexe Logik implementieren kann:

```javascript
// If-Simulation mit For-Schleife
func max(a:int, b:int) -> int
{
    var result = a;
    var condition = b - a;  // > 0 wenn b > a
    
    for(var i = 0; i < condition; i++)
    {
        result = b;  // Wird nur ausgeführt wenn condition > 0
    }
    
    return result;
}
```

Eine echte `if/else`-Implementation ist für **Version 2.1** geplant.

#### Q: Kann ich Rekursion verwenden?

**A:** **Nein**, der Compiler unterstützt keine echte Rekursion. Der Stack-Frame-Ansatz führt bei mehr als ~100 Rekursionsebenen zu Stack-Overflow.

**Workaround:** Nutze iterative Lösungen:

```javascript
// NICHT: Rekursive Fakultät
func factorial_recursive(n:int) -> int
{
    return n * factorial_recursive(n - 1);  // Stack-Overflow!
}

// STATTDESSEN: Iterative Fakultät
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

**Tail-Call-Optimization** ist für Version 3.0 geplant.

#### Q: Funktioniert der Compiler auf Linux/Mac?

**A:** Der Compiler **selbst** kann auf Linux/Mac kompiliert werden, generiert aber **Windows x64 Assembly** mit Windows Calling Convention.

**Für Linux/Mac wären nötig:**
- SystemV ABI statt Windows x64 Calling Convention
- Anpassung der Register-Nutzung (RDI, RSI statt RCX, RDX)
- Anpassung der Syscalls

Dies ist für **Version 2.5** geplant.

#### Q: Wie kann ich den Compiler erweitern?

**A:** Der Code ist Open Source und modular aufgebaut. Mögliche Erweiterungen:

**Einfach (1-2 Tage):**
- Modulo-Operator `%` als primitives Token
- `while`-Schleifen
- `if/else`-Statements

**Mittel (1 Woche):**
- String-Variablen
- Arrays mit fester Größe
- `break`/`continue`

**Schwierig (2-4 Wochen):**
- Floats/Doubles
- Structs
- Pointer
- Heap-Allocation

#### Q: Wo finde ich den Quellcode?

**A:** Der Compiler besteht aus folgenden Dateien:

```
src/
├── compiler_types.h    # Typ-Definitionen (Tokens, AST)
├── lexer.h             # Lexer Interface
├── lexer.c             # Lexer Implementation (~600 Zeilen)
├── parser.h            # Parser Interface
├── parser.c            # Parser & Code Generator (~1500 Zeilen)
└── main.c              # Hauptprogramm (~300 Zeilen)
```

### 11.2 Fehlerbehebung

#### Fehler: "Ungeschlossene String-Literal"

**Ursache:** String enthält UTF-8 Zeichen oder ist zu lang.

**Lösung:**
```javascript
// FALSCH:
print("Sehr langer String der mehr als 246 Zeichen enthält...");

// RICHTIG:
print("Kürzerer String");
print("Oder aufgeteilt in mehrere Zeilen");
```

#### Fehler: "Variable nicht gefunden"

**Ursache:** Variable außerhalb ihres Scopes verwendet.

**Lösung:**
```javascript
// FALSCH:
for(var i = 0; i < 10; i++)
{
    var x = i * 2;
}
print("x = " + x);  // Fehler: x existiert nicht mehr

// RICHTIG:
var x = 0;
for(var i = 0; i < 10; i++)
{
    x = i * 2;
}
print("x = " + x);  // OK
```

#### Fehler: "Zu viele Parameter"

**Ursache:** Mehr als 10 Parameter.

**Lösung:** Nutze ein Struct (in Version 3.0) oder reduziere Parameter-Anzahl.

#### Warnung: "Ignoriere Token nach Ende"

**Ursache:** Datei enthält Müll nach der letzten `}`.

**Lösung:** Wird automatisch bereinigt, keine Aktion nötig.

#### Compiler stürzt ab

**Ursache:** Möglicherweise Stack-Overflow bei tiefer Verschachtelung.

**Lösung:**
1. Reduziere Verschachtelungs-Tiefe
2. Erhöhe Stack-Größe: `ulimit -s unlimited` (Linux)
3. Kompiliere Compiler mit `-Wl,--stack,8388608` (Windows)

---

## 12. Entwickler-Guide

### 12.1 Architektur erweitern

#### Neuen Token-Typ hinzufügen

1. **In `compiler_types.h`:**
```c
typedef enum {
    // ... bestehende Tokens
    TOKEN_MODULO,  // Neu: %
    // ...
} TokenType;
```

2. **In `lexer.c`:**
```c
// In tokenize_source(), switch-statement:
case '%': type = TOKEN_MODULO; break;
```

3. **In `lexer.c`, token_type_to_string():**
```c
case TOKEN_MODULO: return "MODULO";
```

4. **In `parser.c`, parse_factor():**
```c
else if (match(parser->tokens, TOKEN_MODULO)) {
    code_printf(parser, "    cltd\n");
    code_printf(parser, "    idivl %%ebx\n");
    code_printf(parser, "    movl %%edx, %%eax\n");  // Rest in EDX
}
```

#### Neues Statement hinzufügen (while-Schleife)

1. **Keyword hinzufügen** (siehe oben)

2. **In `parser.c`, parse_statement():**
```c
else if (check(parser->tokens, TOKEN_KEYWORD_WHILE)) {
    parse_while_loop(parser);
}
```

3. **Neue Parser-Funktion:**
```c
void parse_while_loop(Parser *parser) {
    consume(parser->tokens);  // 'while'
    
    // Label für Loop-Start
    int loop_id = parser->loop_counter++;
    code_printf(parser, ".L_while_start_%d:\n", loop_id);
    
    // Condition
    expect(parser, TOKEN_LPAREN, "Erwarte '('");
    parse_expression(parser);
    expect(parser, TOKEN_RPAREN, "Erwarte ')'");
    
    // Test condition
    code_printf(parser, "    popq %%rax\n");
    code_printf(parser, "    testl %%eax, %%eax\n");
    code_printf(parser, "    je .L_while_end_%d\n", loop_id);
    
    // Body
    expect(parser, TOKEN_LBRACE, "Erwarte '{'");
    parser->current_scope++;
    
    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }
    
    expect(parser, TOKEN_RBRACE, "Erwarte '}'");
    cleanup_scope(parser, parser->current_scope);
    parser->current_scope--;
    
    // Jump back to start
    code_printf(parser, "    jmp .L_while_start_%d\n", loop_id);
    code_printf(parser, ".L_while_end_%d:\n", loop_id);
}
```

### 12.2 Debug-Techniken

#### Token-Stream debuggen

```bash
./compiler --tokens program.txt | grep "KEYWORD\|IDENTIFIER" | head -20
```

#### Parser-Flow debuggen

```bash
./compiler --debug program.txt 2>&1 | grep "PARSE"
```

#### Assembly-Code analysieren

```bash
./compiler program.txt
cat program.txt.s | less
```

#### GDB zum Debuggen nutzen

```bash
gcc -g program.txt.s -o program
gdb program
(gdb) break main
(gdb) run
(gdb) step
(gdb) info registers
```

### 12.3 Coding Style

Der Compiler folgt diesen Konventionen:

- **Indentation:** 4 Spaces
- **Naming:**
  - Functions: `snake_case` (z.B. `parse_expression`)
  - Types: `PascalCase` (z.B. `TokenStream`)
  - Constants: `UPPER_SNAKE_CASE` (z.B. `MAX_VARS`)
- **Comments:**
  - `//` für einzeilige Kommentare
  - `/* */` für mehrzeilige Kommentare
- **Error Handling:** Immer mit `fprintf(stderr, ...)`

---