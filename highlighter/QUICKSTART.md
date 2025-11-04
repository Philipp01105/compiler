# Quick Start Guide - CLion Highlighter Plugin

## ⚡ Fast Track Installation

### 1. Build the Plugin (30 seconds)

**On Linux/Mac:**
```bash
cd highlighter
./gradlew buildPlugin
```

**On Windows:**
```cmd
cd highlighter
gradlew.bat buildPlugin
```

The plugin ZIP will be created at:
```
highlighter/build/distributions/compiler-highlighter-1.0.0.zip
```

### 2. Install in CLion (1 minute)

1. Open **CLion**
2. Go to **File** → **Settings** → **Plugins**
3. Click **⚙️** (gear icon) → **Install Plugin from Disk...**
4. Select `compiler-highlighter-1.0.0.zip`
5. Click **OK** and **Restart** CLion

### 3. Test It!

1. Open `test.txt` from the compiler project
2. You should see colored syntax highlighting:
   - **Purple/Blue** keywords: `func`, `var`, `if`, `else`, `for`, `return`, `struct`
   - **Teal/Cyan** types: `int`, `string`, `float`, `void`, `char`, `byte`, `bit`, `double`
   - **Green** strings: `"Hello World"`
   - **Blue** numbers: `42`, `3.14`
   - **Gray** comments: `// This is a comment`

## 🎨 Quick Customization

To change colors:
1. **Settings** → **Editor** → **Color Scheme** → **Custom Compiler Language**
2. Select any element (Keyword, Type, String, etc.)
3. Change foreground/background color
4. Click **Apply**

## 📋 Example Code

Copy this into a `.txt` file to see highlighting in action:

```javascript
// Factorial function
func factorial(n:int) -> int {
    var result:int = 1;
    for (var i:int = 1; i <= n; i++) {
        result = result * i;
    }
    return result;
}

// Main function
func main() -> void {
    var name:string = "World";
    var age:int = 25;
    var pi:float = 3.14159;
    
    print("Hello, " + name + "!");
    print("Age: " + age);
    
    if (age >= 18) {
        print("Adult");
    } else {
        print("Minor");
    }
    
    var fact5:int = factorial(5);
    print("5! = " + fact5);
}

// Point struct with methods
struct Point {
    int x;
    int y;
    
    func move(dx:int, dy:int) -> void {
        x = x + dx;
        y = y + dy;
    }
}
```

## 🔧 Troubleshooting

**Plugin won't load?**
- Ensure Java 17+ is installed: `java -version`
- Check CLion version is 2023.2 or newer

**No syntax highlighting?**
- Make sure file has `.txt` extension
- Close and reopen the file
- Check if plugin is enabled in Settings → Plugins

**Build failed?**
```bash
./gradlew clean
./gradlew buildPlugin
```

## 📚 Full Documentation

For complete documentation, see:
- **README.md** - Full feature documentation (English)
- **INSTALL.md** - Detailed installation guide (German)

## 🚀 Features

✅ **All 7 Data Types:** int, char, byte, bit, float, double, string  
✅ **Keywords:** func, var, if, else, for, return, struct  
✅ **Operators:** +, -, *, /, %, ==, !=, <, >, <=, >=, &&, ||, !  
✅ **String & Char Literals:** "text" and 'c'  
✅ **Numbers:** 42, 3.14, 1.5e10  
✅ **Comments:** // line comments  
✅ **Structs with Methods:** OOP-like features  
✅ **Customizable Colors:** Via CLion settings  

## 📝 Language Features Supported

The highlighter recognizes all features of the custom compiler language version 5.1.0:
- Type inference: `var x = 5;`
- Explicit types: `var x:int = 5;`
- Functions: `func add(a:int, b:int) -> int`
- Control flow: `if`, `else if`, `else`, `for` loops
- Structs: `struct Point { int x; int y; }`
- Methods: `func move(dx:int, dy:int) -> void { ... }`
- Arrays: `var[10] nums:int;`
- String operations: `print("Hello, " + name);`

## 🎯 Version Info

**Plugin Version:** 1.0.0  
**Compatible with:** CLion 2023.2+  
**Compiler Version:** 5.1.0  
**Date:** 2025-11-04  
**Author:** Philipp01105

---

**Need help?** Open an issue at https://github.com/Philipp01105/compiler/issues
