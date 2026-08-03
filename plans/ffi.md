# Native `extern`-Blöcke und System-FFI für DMM

## Ziel

DMM erhält eine kleine, allgemeine Native-FFI, mit der Packages Funktionen und Datenlayouts nativer Systembibliotheken deklarieren können.

Die FFI soll insbesondere ermöglichen, Plattform-APIs wie Windows Winsock, Kernel32 oder Linux-Systembibliotheken als normale DMM-Packages abzubilden, ohne einzelne Betriebssystemfunktionen als Compiler-Intrinsics implementieren zu müssen.

Beispiel:

```dmm
package win32.winsock;

extern "system" from "ws2_32.dll" {
    pub struct SOCKADDR {
        family:u16;
        data:u8[14];
    }

    pub func socket(
        af:i32,
        type:i32,
        protocol:i32
    ) -> usize;

    pub func closesocket(socket:usize) -> i32;
}
```

Anwender verwenden diese Deklarationen wie normale Package-Symbole:

```dmm
import "win32/winsock";

var handle = winsock.socket(
    winsock.AF_INET,
    winsock.SOCK_STREAM,
    winsock.IPPROTO_TCP
);
```

Ob ein Symbol in DMM oder einer nativen Bibliothek implementiert ist, ist für den Aufrufer nicht relevant.

---

## 1. Sprachmodell

### 1.1 `extern`-Block

Neue Syntax:

```dmm
extern "system" from "library" {
    declarations...
}
```

Ein `extern`-Block deklariert eine native ABI-Grenze.

Die Eigenschaften bleiben voneinander unabhängig:

- `extern` bedeutet, dass die Implementierung außerhalb von DMM liegt.
- `"system"` bestimmt die Calling Convention.
- `from "..."` bestimmt die native Bibliothek, aus der Funktionssymbole importiert werden.
- `pub` verwendet die normalen DMM-Sichtbarkeitsregeln.

Beispiel:

```dmm
extern "system" from "ws2_32.dll" {
    pub func WSAGetLastError() -> i32;

    func privateNativeHelper() -> i32;
}
```

`WSAGetLastError` ist für importierende Packages sichtbar. `privateNativeHelper` bleibt package-intern.

---

## 2. ABI-Auswahl

Zunächst wird ausschließlich

```dmm
extern "system"
```

unterstützt.

`system` bedeutet die native System-ABI des aktuellen Targets.

Für die bestehenden Targets:

```text
x86_64 Windows -> Windows x64 ABI
x86_64 Linux   -> System V AMD64 ABI
```

Die Source-Syntax beschreibt damit keine konkrete Architektur.

Spätere ABI-Bezeichner wie

```dmm
extern "C"
```

können ergänzt werden, wenn dafür ein konkreter Anwendungsfall besteht.

Unbekannte ABI-Bezeichner sind Compilerfehler.

---

## 3. Library-Angabe

Native Funktionen können an eine Bibliothek gebunden werden:

```dmm
extern "system" from "ws2_32.dll" {
    pub func WSAGetLastError() -> i32;
}
```

`from` gilt für alle Funktionsdeklarationen des Blocks.

Die Bibliotheksangabe wird als Link-/Import-Metadatum behandelt und ist kein DMM-Package-Import.

Mehrere Blöcke dürfen dieselbe Bibliothek verwenden.

Beispiel:

```dmm
extern "system" from "kernel32.dll" {
    // ...
}

extern "system" from "ws2_32.dll" {
    // ...
}
```

---

## 4. Externe Funktionen

Eine Funktion innerhalb eines `extern`-Blocks besitzt keinen DMM-Body:

```dmm
extern "system" from "ws2_32.dll" {
    pub func socket(
        af:i32,
        type:i32,
        protocol:i32
    ) -> usize;
}
```

Folgende Form ist unzulässig:

```dmm
extern "system" from "ws2_32.dll" {
    func socket(...) -> usize {
        // Fehler: extern functions besitzen keinen Body.
    }
}
```

Der Compiler erzeugt für die Deklaration keine Implementierung.

Stattdessen wird ein Native-Symbol mit folgenden Informationen erzeugt:

```text
NativeImport {
    sourceName
    nativeName
    library
    callingConvention
    functionType
    visibility
}
```

Für das Beispiel:

```text
sourceName        = win32.winsock.socket
nativeName        = socket
library           = ws2_32.dll
callingConvention = system
```

---

## 5. Native Structs

Structs dürfen innerhalb eines `extern`-Blocks deklariert werden:

```dmm
extern "system" from "ws2_32.dll" {
    pub struct SOCKADDR {
        family:u16;
        data:u8[14];
    }
}
```

Ein solches Struct ist ein **native ABI struct**.

Es ist kein Symbol der DLL. `from "ws2_32.dll"` beeinflusst ausschließlich importierbare Symbole.

Für ein externes Struct garantiert DMM:

- deterministisches natives Layout,
- targetabhängiges ABI-Alignment,
- ABI-konformes Padding,
- keine versteckten DMM-Felder,
- keine Ownership-Metadaten,
- keine Drop-Glue,
- keine automatische Layout-Optimierung oder Feldumordnung.

`sizeof`, Alignment und Feldoffsets müssen dem jeweiligen nativen ABI entsprechen.

---

## 6. Library-unabhängige Native-Typen

Native Typen können auch ohne `from` deklariert werden:

```dmm
extern "system" {
    pub struct OVERLAPPED {
        // ...
    }
}
```

Damit können Typen von mehreren Bibliotheken verwendet werden:

```dmm
extern "system" {
    pub struct OVERLAPPED {
        // exaktes natives Layout
    }
}

extern "system" from "kernel32.dll" {
    pub func CancelIoEx(
        handle:*void,
        overlapped:*OVERLAPPED
    ) -> bit;
}
```

Ein `extern`-Block ohne `from` darf keine Funktion deklarieren, die ein ungelöstes natives Importsymbol benötigt.

---

## 7. FFI-sichere Typen

Extern-Deklarationen dürfen zunächst ausschließlich eine definierte Menge ABI-sicherer Typen verwenden.

Erlaubt:

- `bit`
- `char`
- `i8`, `u8`
- `i16`, `u16`
- `i32`, `u32`
- `i64`, `u64`
- `isize`, `usize`
- `float`, `double`
- rohe Pointer
- `void`
- Arrays fester Länge innerhalb nativer Structs
- andere native ABI structs

Beispiel:

```dmm
extern "system" {
    struct NativeData {
        value:u32;
        handle:*void;
        bytes:u8[16];
    }
}
```

Nicht FFI-sicher sind zunächst insbesondere:

- `string`
- Slices
- Closures
- Interfaces
- `Option`
- `Result`
- normale owning DMM-Aggregate mit Drop-Semantik
- Futures
- Referenzen mit DMM-Borrow-Semantik

Beispiel:

```dmm
extern "system" {
    struct Invalid {
        name:string;       // Fehler
        values:int[];      // Fehler
    }
}
```

Die FFI führt keine impliziten Marshaling-Operationen durch.

---

## 8. Pointer und Safety-Grenze

Native Funktionen arbeiten mit rohen Pointern.

Beispiel:

```dmm
extern "system" from "ws2_32.dll" {
    pub func WSAStartup(
        version:u16,
        data:*WSADATA
    ) -> i32;
}
```

Ein Aufruf:

```dmm
var data:winsock.WSADATA;
var result = winsock.WSAStartup(0x0202, &data);
```

überschreitet die native Safety-Grenze.

Der Compiler garantiert dabei ABI-korrekte Übergabe, aber nicht:

- Gültigkeit fremder Pointer,
- native Lifetime-Anforderungen,
- Thread-Sicherheit der API,
- Ownership fremder Handles,
- Semantik nativer Fehlercodes.

Diese Verantwortung liegt beim Low-Level-Package, das auf der FFI aufbaut.

---

## 9. Package-Modell

Native Bindings sind normale DMM-Packages.

Beispiel:

```dmm
package win32.winsock;

pub const AF_INET:i32 = 2;
pub const SOCK_STREAM:i32 = 1;
pub const IPPROTO_TCP:i32 = 6;
pub const INVALID_SOCKET:usize = usize.max;

extern "system" from "ws2_32.dll" {
    pub struct WSADATA {
        // exaktes natives Layout
    }

    pub struct SOCKADDR {
        family:u16;
        data:u8[14];
    }

    pub func WSAStartup(
        version:u16,
        data:*WSADATA
    ) -> i32;

    pub func WSACleanup() -> i32;
    pub func WSAGetLastError() -> i32;

    pub func socket(
        af:i32,
        type:i32,
        protocol:i32
    ) -> usize;

    pub func bind(
        socket:usize,
        address:*SOCKADDR,
        addressLength:i32
    ) -> i32;

    pub func listen(
        socket:usize,
        backlog:i32
    ) -> i32;

    pub func closesocket(
        socket:usize
    ) -> i32;
}
```

Anwendung:

```dmm
package main;

import "win32/winsock";

func main() -> int {
    var data:winsock.WSADATA;

    var startup = winsock.WSAStartup(
        0x0202,
        &data
    );

    if (startup != 0) {
        return startup;
    }

    var handle = winsock.socket(
        winsock.AF_INET,
        winsock.SOCK_STREAM,
        winsock.IPPROTO_TCP
    );

    if (handle == winsock.INVALID_SOCKET) {
        var error = winsock.WSAGetLastError();
        winsock.WSACleanup();
        return error;
    }

    winsock.closesocket(handle);
    winsock.WSACleanup();

    return 0;
}
```

Der Aufrufer behandelt `winsock.socket` wie jede andere importierte Funktion.

---

## 10. Sichere Wrapper

Raw-FFI-Packages sollen keine High-Level-API darstellen.

Darüber werden normale DMM-Typen gebaut:

```text
ws2_32.dll
      ↑
extern ABI
      ↑
win32.winsock
      ↑
stdlib/core/net
      ↑
stdlib/net
      ↑
application
```

Beispiel:

```dmm
struct Socket {
    private var handle:usize;

    destructor {
        if (handle != winsock.INVALID_SOCKET) {
            winsock.closesocket(handle);
        }
    }
}
```

Damit bleiben native Handles und Pointer an der unteren Schicht, während höhere APIs DMM-Ownership, Borrows, `Result`, Destruktoren und Async verwenden können.

---

## 11. Frontend

Lexer und Parser erhalten Unterstützung für:

```text
extern
from
```

sowie String-ABI- und Library-Angaben.

AST-Erweiterungen:

```text
AST_EXTERN_BLOCK
AST_EXTERN_FUNCTION
AST_EXTERN_STRUCT
```

Ein Extern-Block speichert mindestens:

```text
abi
optional library
declarations
source location
```

Extern-Funktionen speichern zusätzlich ihren nativen Symbolnamen.

Parser-Diagnosen müssen mindestens unterscheiden:

- unbekannte ABI,
- fehlende Library bei importierter Funktion,
- Function Body in extern function,
- ungültige Deklaration innerhalb eines extern-Blocks.

---

## 12. Semantische Analyse

Die Sema prüft:

1. ABI wird vom Target unterstützt.
2. Extern-Funktionen besitzen keinen Body.
3. Parameter und Rückgabetyp sind FFI-sicher.
4. Native Struct-Felder sind FFI-sicher.
5. Native Structs besitzen keine DMM-spezifische Drop-Semantik.
6. Sichtbarkeit folgt normalen Package-Regeln.
7. Native Symbole sind innerhalb ihrer Library eindeutig auflösbar.
8. Target und native Bibliothek sind kompatibel.

Plattformspezifische Packages können zusätzlich über das Package-/Manifest-System auf unterstützte Targets begrenzt werden.

---

## 13. IR

Die IR unterscheidet DMM-Funktionen und native Imports.

Beispiel:

```text
IR_FUNCTION
    kind = NATIVE_IMPORT
    abi = SYSTEM
    library = "ws2_32.dll"
    symbol = "socket"
    signature = ...
```

Alternativ kann dafür ein eigener Symboltyp verwendet werden:

```text
IR_NATIVE_IMPORT
```

Der IR-Verifier prüft:

- gültige ABI,
- vollständige Signatur,
- FFI-sichere Typen,
- vorhandene Library,
- keine DMM-Implementierung für dasselbe Importsymbol.

Calls verwenden danach den normalen Call-Lowering-Pfad mit der angegebenen ABI.

---

## 14. Backend

Das x86-64-Backend verwendet die bestehende ABI-Klassifikation.

Für:

```dmm
extern "system"
```

gilt:

```text
Windows x86-64
    -> Windows x64 calling convention

Linux x86-64
    -> System V AMD64 calling convention
```

Damit müssen Argumentregister, Stack-Layout, Return-Werte, Aggregate und Alignment genauso behandelt werden wie bei entsprechenden nativen Calls.

Extern-Calls dürfen nicht über die DMM-interne Funktions-ABI abgesenkt werden.

---

## 15. Linker-Metadaten

Die Codegenerierung sammelt alle tatsächlich verwendeten Native-Imports.

Beispiel:

```text
NativeLibrary:
    ws2_32.dll

Imports:
    WSAStartup
    socket
    closesocket
```

Nicht verwendete Deklarationen erzeugen keine notwendige Runtime-/Library-Abhängigkeit, sofern sie nach der finalen IR-/Reachability-Analyse nicht benötigt werden.

Dadurch gilt:

```dmm
import "win32/winsock";
```

allein nicht als Verwendung von `ws2_32.dll`.

Erst ein erreichbarer nativer Call erzeugt die Linkanforderung.

---

## 16. Externer Linkpfad

Im ersten Implementierungsschritt verwendet Native-FFI den bestehenden externen Linker-Handoff.

Der Compiler übersetzt benötigte Libraries in die passende Linker-Anforderung.

Beispiel Windows:

```text
DMM native import
        ↓
ws2_32.dll
        ↓
external link requirement
        ↓
MinGW GCC/Clang
        ↓
ws2_32 import
```

Die genaue Übersetzung von DLL-Namen zu Linkerargumenten ist Teil des Target-/Treiber-Layers und nicht der Sprachsemantik.

---

## 17. Zukünftiger interner Linker

Das Source-Modell darf nicht vom externen Linker abhängen.

Später kann der interne PE-Linker dieselben Native-Import-Metadaten direkt verarbeiten:

```text
NativeImport
    library = ws2_32.dll
    symbol = socket
            ↓
internal PE linker
            ↓
PE Import Directory
Import Lookup Table
Import Address Table
            ↓
program.exe
```

DMM-Quellcode und Packages bleiben dabei unverändert.

Damit sind

```text
RuntimeProfile
LinkMode
NativeImports
```

getrennte Konzepte.

---

## 18. Symbol-Aliase

Für APIs, deren nativer Symbolname nicht dem gewünschten DMM-Namen entspricht, soll eine explizite Umbenennung vorgesehen werden.

Vorgeschlagene Syntax:

```dmm
extern "system" from "user32.dll" {
    pub func messageBox(...) -> i32
        = "MessageBoxW";
}
```

Intern:

```text
sourceName = messageBox
nativeName = MessageBoxW
```

Es findet keine automatische Namensänderung statt.

Dieses Feature kann nach der grundlegenden FFI-Unterstützung implementiert werden, das interne Datenmodell soll `sourceName` und `nativeName` jedoch von Anfang an trennen.

---

## 19. Bewusst nicht enthalten

Die erste FFI-Version enthält nicht:

- dynamisches `LoadLibrary`/`GetProcAddress`,
- Callback-Trampolines,
- C-Variadics,
- C-Bitfields,
- C++ ABI,
- automatische Header-Übersetzung,
- automatische String-Konvertierung,
- automatische Ownership-Übernahme,
- native Exceptions,
- allgemeines C-Makro-System,
- automatische Bindgen-Funktionalität.

Diese Features benötigen eigene Designs.

---

## 20. Tests

### Parser

- gültige extern-Blöcke,
- Block mit und ohne `from`,
- mehrere Libraries,
- `pub` und private Deklarationen,
- ungültige ABI,
- Function Body in extern function,
- ungültige Blockdeklarationen.

### Typprüfung

- primitive FFI-Typen,
- Pointer,
- Arrays,
- verschachtelte native Structs,
- Ablehnung von `string`,
- Ablehnung von Slices,
- Ablehnung von Closures,
- Ablehnung von owning DMM-Typen,
- Layout- und Alignment-Prüfungen.

### ABI

Unter Windows x86-64:

- Integerargumente,
- Pointer,
- Floating Point,
- Stackargumente,
- native Structs,
- Return-Werte.

Unter Linux x86-64 dieselben Tests mit System-V-ABI.

### Linking

- verwendete native Library wird eingebunden,
- ungenutzter Import erzeugt keine Abhängigkeit,
- fehlende Library liefert klare Diagnose,
- fehlendes Symbol liefert klare Linkdiagnose,
- Objekt- und Assembly-Ausgabe bleiben linkbar,
- O0/O1,
- Windows UCRT64 GCC/Clang,
- Linux GCC/Clang.

### Runtime

Mindestens ein echter Plattformtest:

```text
Windows:
WSAStartup
socket
closesocket
WSACleanup
```

und ein entsprechender kleiner Linux-System-API-Test.

---

## 21. Dokumentation

Folgende Dokumente werden erweitert:

- `LANGUAGE_SPEC.md`
    - extern blocks
    - ABI
    - native structs
    - FFI-safe types
    - Safety-Grenze

- `FORMAL_LANGUAGE.md`
    - Grammatik für extern-Blöcke und native Deklarationen

- `NATIVE_BACKEND.md`
    - NativeImport-Repräsentation
    - ABI-Lowering
    - Linkeranforderungen
    - externer und zukünftiger interner Linkpfad

Zusätzlich erhält die Stdlib-/Package-Dokumentation die Schichten:

```text
raw platform bindings
        ↓
core wrappers
        ↓
safe/high-level APIs
```

---

## 22. Implementierungsreihenfolge

1. Grammatik und AST für `extern "system"`.
2. `from "library"` und Sichtbarkeit.
3. Extern-Funktionsdeklarationen.
4. FFI-safe primitive Typprüfung.
5. Native Structs und ABI-Layout.
6. NativeImport-Repräsentation in der IR.
7. System-ABI-Call-Lowering.
8. Sammlung tatsächlich verwendeter Libraries/Symbole.
9. Integration in den externen Linker-Handoff.
10. Windows-Test mit einer kleinen System-DLL.
11. Linux-Test mit einer nativen Systembibliothek.
12. Raw-Winsock-Package als erster größerer Realitätscheck.
13. Dokumentation und vollständige Regression-Suite.
14. Optional anschließend Symbol-Aliase.
15. Später interne PE-/ELF-Dynamic-Imports.

---

## Definition of Done

Das Feature ist abgeschlossen, wenn ein DMM-Package eine native Systembibliothek ausschließlich über DMM-Quellcode deklarieren und verwenden kann:

```dmm
extern "system" from "ws2_32.dll" {
    pub struct NativeType {
        // ABI-kompatible Felder
    }

    pub func nativeFunction(
        value:*NativeType
    ) -> i32;
}
```

und ein anderes Package diese Deklaration normal importieren kann:

```dmm
import "win32/winsock";

var result = winsock.nativeFunction(&value);
```

Der Compiler muss dabei:

- Typen und native Layouts prüfen,
- die korrekte Target-ABI verwenden,
- das native Symbol und seine Library verfolgen,
- nur tatsächlich benötigte native Abhängigkeiten linken,
- verständliche Fehler für ungültige FFI-Typen liefern,
- Windows x86-64 und Linux x86-64 unterstützen,
- den bestehenden Standalone-Pfad für Programme ohne Native-Imports unverändert lassen.

Die erste Version bleibt bewusst eine rohe, unsichere ABI-Grenze. Ownership, Fehlerübersetzung, Borrows, Async und sichere Ressourcenverwaltung werden von normalen DMM-Packages oberhalb dieser Grenze implementiert.