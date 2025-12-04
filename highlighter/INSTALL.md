# Installation Guide - Custom Compiler Language Highlighter Plugin für CLion

Diese Anleitung erklärt Schritt für Schritt, wie das CLion Syntax-Highlighter-Plugin für die Custom Compiler Language gebaut und in CLion installiert wird.

## 📋 Voraussetzungen

Bevor Sie beginnen, stellen Sie sicher, dass folgende Software installiert ist:

### 1. Java Development Kit (JDK)
- **Version:** JDK 17 oder höher
- **Download:** https://adoptium.net/ oder https://www.oracle.com/java/technologies/downloads/

**Überprüfung der Installation:**
```bash
java -version
```
Sollte etwas wie `openjdk version "17.0.x"` oder höher anzeigen.

### 2. Gradle (Optional - wird automatisch heruntergeladen)
- Das Projekt enthält den Gradle Wrapper, der automatisch die richtige Gradle-Version herunterlädt
- **Manuelle Installation:** https://gradle.org/install/ (nur wenn gewünscht)

### 3. CLion IDE
- **Version:** 2023.2 oder höher
- **Download:** https://www.jetbrains.com/clion/download/

---

## 🔨 Schritt 1: Plugin bauen

### Windows

1. **CMD oder PowerShell öffnen**

2. **Zum highlighter-Verzeichnis navigieren:**
   ```cmd
   cd C:\Pfad\zum\compiler\highlighter
   ```

3. **Plugin bauen:**
   ```cmd
   gradlew.bat buildPlugin
   ```

   Oder mit Gradle Wrapper:
   ```cmd
   .\gradlew.bat buildPlugin
   ```

### Linux / macOS

1. **Terminal öffnen**

2. **Zum highlighter-Verzeichnis navigieren:**
   ```bash
   cd /pfad/zum/compiler/highlighter
   ```

3. **Gradle Wrapper ausführbar machen (beim ersten Mal):**
   ```bash
   chmod +x gradlew
   ```

4. **Plugin bauen:**
   ```bash
   ./gradlew buildPlugin
   ```

### Was passiert beim Build?

Der Build-Prozess:
1. Lädt alle notwendigen Dependencies herunter
2. Kompiliert den Java-Code
3. Erstellt die Plugin-Struktur
4. Packt alles in eine ZIP-Datei

**Build-Ausgabe:** Das fertige Plugin liegt in:
```
highlighter/build/distributions/compiler-highlighter-1.0.0.zip
```

---

## 📦 Schritt 2: Plugin in CLion installieren

### Methode A: Installation vom Datenträger (Empfohlen)

1. **CLion öffnen**

2. **Einstellungen öffnen:**
   - **Windows/Linux:** Menü → `File` → `Settings`
   - **macOS:** Menü → `CLion` → `Preferences`
   - **Tastenkombination:** `Ctrl+Alt+S` (Windows/Linux) oder `Cmd+,` (macOS)

3. **Zu Plugins navigieren:**
   - Im linken Menü `Plugins` auswählen

4. **Plugin von Datenträger installieren:**
   - Klicken Sie auf das **⚙️ Zahnrad-Symbol** oben rechts
   - Wählen Sie **"Install Plugin from Disk..."**
   
   ![Plugin Installation](https://i.imgur.com/example.png)

5. **ZIP-Datei auswählen:**
   - Navigieren Sie zu: `highlighter/build/distributions/`
   - Wählen Sie: `compiler-highlighter-1.0.0.zip`
   - Klicken Sie auf **OK**

6. **CLion neu starten:**
   - CLion wird Sie auffordern, die IDE neu zu starten
   - Klicken Sie auf **"Restart"**

### Methode B: Manuelle Installation

1. **Plugin-ZIP entpacken:**
   ```bash
   unzip compiler-highlighter-1.0.0.zip -d compiler-highlighter
   ```

2. **Plugin-Ordner in CLion-Plugins-Verzeichnis kopieren:**
   - **Windows:** `%APPDATA%\JetBrains\CLion2023.2\plugins\`
   - **Linux:** `~/.local/share/JetBrains/CLion2023.2/plugins/`
   - **macOS:** `~/Library/Application Support/JetBrains/CLion2023.2/plugins/`

3. **CLion neu starten**

---

## ✅ Schritt 3: Plugin testen

### 3.1 Überprüfen, ob das Plugin geladen ist

1. **Einstellungen öffnen:** `File` → `Settings` → `Plugins`
2. Im Tab **"Installed"** sollte "**Custom Compiler Language Support**" erscheinen
3. Das Plugin sollte aktiviert sein (Häkchen gesetzt)

### 3.2 Syntax-Highlighting testen

1. **Testdatei öffnen:**
   - Öffnen Sie `test.txt` aus dem Hauptverzeichnis des Compilers
   - Oder erstellen Sie eine neue `.txt`-Datei

2. **Beispielcode einfügen:**
   ```javascript
   // Dies ist ein Kommentar
   func main() -> void {
       var name:string = "World";
       var age:int = 25;
       var pi:float = 3.14;
       
       print("Hello, " + name + "!");
       
       if (age >= 18) {
           print("Adult");
       } else {
           print("Child");
       }
       
       for (var i:int = 0; i < 10; i++) {
           print("Iteration: " + i);
       }
   }
   ```

3. **Syntax-Highlighting überprüfen:**
   - **Keywords** (`func`, `var`, `if`, `else`, `for`, `return`) sollten farblich hervorgehoben sein
   - **Types** (`int`, `string`, `float`, `void`) sollten in einer anderen Farbe erscheinen
   - **String-Literale** (`"Hello, " + name + "!"`) sollten grün sein
   - **Zahlen** (`25`, `3.14`, `0`, `10`) sollten blau sein
   - **Kommentare** (`// Dies ist ein Kommentar`) sollten grau/kursiv sein
   - **Operatoren** (`+`, `=`, `>=`, `<`) sollten hervorgehoben sein

---

## 🎨 Schritt 4: Farben anpassen (Optional)

Sie können die Farben des Syntax-Highlightings nach Ihren Wünschen anpassen:

1. **Einstellungen öffnen:** `File` → `Settings`

2. **Color Scheme navigieren:**
   - `Editor` → `Color Scheme` → `Custom Compiler Language`

3. **Farben für jedes Element anpassen:**
   - **Keyword** - Schlüsselwörter wie `func`, `var`, `if`
   - **Type** - Datentypen wie `int`, `string`, `float`
   - **String** - String-Literale
   - **Character** - Character-Literale
   - **Number** - Zahlenliterale
   - **Operator** - Operatoren
   - **Punctuation** - Satzzeichen
   - **Comment** - Kommentare
   - **Identifier** - Bezeichner (Variablen, Funktionen)

4. **Vorschau:**
   - Die Änderungen werden sofort in der Vorschau angezeigt
   - Klicken Sie auf **"Apply"**, um die Änderungen zu speichern

---

## 🔧 Troubleshooting (Fehlerbehebung)

### Problem: "Plugin konnte nicht geladen werden"

**Lösung:**
1. Überprüfen Sie die CLion-Version (mindestens 2023.2 erforderlich)
2. Stellen Sie sicher, dass Java 17+ installiert ist
3. Prüfen Sie die Logs: `Help` → `Show Log in Explorer/Finder`
4. Deinstallieren Sie das Plugin und installieren Sie es erneut

### Problem: "Build schlägt fehl"

**Lösung:**
```bash
# Cache löschen und neu bauen
./gradlew clean
./gradlew buildPlugin
```

Wenn das nicht hilft:
```bash
# Gradle-Wrapper neu herunterladen
./gradlew wrapper --gradle-version=8.4
./gradlew buildPlugin
```

### Problem: "Syntax wird nicht hervorgehoben"

**Lösung:**
1. Überprüfen Sie, ob die Datei die Erweiterung `.txt` hat
2. Schließen Sie die Datei und öffnen Sie sie erneut
3. Gehen Sie zu `File` → `Settings` → `Editor` → `File Types`
4. Stellen Sie sicher, dass `.txt` mit "Custom Compiler Language" verknüpft ist
5. Wenn nicht: Klicken Sie auf "Custom Compiler Language" → `+` → `*.txt` hinzufügen

### Problem: "Farben sehen seltsam aus"

**Lösung:**
1. Gehen Sie zu `Settings` → `Editor` → `Color Scheme` → `Custom Compiler Language`
2. Klicken Sie auf **"Restore Defaults"**
3. Oder passen Sie die Farben manuell an

### Problem: "CLion findet Java nicht"

**Lösung:**
1. Überprüfen Sie, ob `JAVA_HOME` gesetzt ist:
   ```bash
   echo $JAVA_HOME    # Linux/macOS
   echo %JAVA_HOME%   # Windows
   ```

2. Falls nicht, setzen Sie es:
   - **Windows:** Systemeinstellungen → Erweiterte Systemeinstellungen → Umgebungsvariablen
   - **Linux/macOS:** Fügen Sie zu `~/.bashrc` oder `~/.zshrc` hinzu:
     ```bash
     export JAVA_HOME=/pfad/zu/java
     ```

---

## 🔄 Plugin aktualisieren

Wenn Sie das Plugin aktualisieren möchten:

1. **Neuen Code pullen:**
   ```bash
   git pull origin main
   ```

2. **Neu bauen:**
   ```bash
   cd highlighter
   ./gradlew clean buildPlugin
   ```

3. **In CLion:**
   - `File` → `Settings` → `Plugins`
   - Altes Plugin deinstallieren
   - Neues Plugin installieren (siehe Schritt 2)
   - CLion neu starten

---

## 📝 Weitere Informationen

### Plugin-Struktur

```
highlighter/
├── build.gradle.kts              # Build-Konfiguration
├── settings.gradle.kts           # Gradle-Einstellungen
├── README.md                     # Englische Dokumentation
├── INSTALL.md                    # Diese Datei (Deutsch)
└── src/main/
    ├── java/                     # Java-Quellcode
    │   └── com/philipp/compiler/highlighter/
    │       ├── CompilerLanguage.java
    │       ├── CompilerFileType.java
    │       ├── CompilerLexer.java
    │       ├── CompilerSyntaxHighlighter.java
    │       └── ... (weitere Klassen)
    └── resources/
        └── META-INF/
            └── plugin.xml        # Plugin-Konfiguration
```

### Unterstützte Sprachfeatures

Das Plugin unterstützt vollständiges Syntax-Highlighting für:

- ✅ **7 Datentypen:** int, char, byte, bit, float, double, string
- ✅ **Schlüsselwörter:** func, var, if, else, for, return, struct
- ✅ **Operatoren:** +, -, *, /, %, ==, !=, <, >, <=, >=, &&, ||, !
- ✅ **String-Literale:** "Hello World"
- ✅ **Character-Literale:** 'A'
- ✅ **Zahlen:** 42, 3.14, 1.5e10
- ✅ **Kommentare:** // Zeilenkommentare
- ✅ **Structs mit Methoden:** struct Point { ... }
- ✅ **Arrays:** var[10] nums:int

---

## 📞 Support

Bei Problemen oder Fragen:

- **GitHub Issues:** https://github.com/Philipp01105/compiler/issues
- **Repository:** https://github.com/Philipp01105/compiler

---

## 📜 Lizenz

Dieses Plugin ist Teil des Compiler-Projekts von Philipp01105.

**Version:** 1.0.0  
**Datum:** 2025-11-04  
**Autor:** Philipp01105  
**Compiler-Version:** 5.1.0

---

**Viel Erfolg beim Verwenden des Plugins! 🚀**
