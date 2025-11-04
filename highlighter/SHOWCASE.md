# Syntax Highlighting Showcase

This document demonstrates how the CLion highlighter plugin highlights code from the custom compiler language.

## 🎨 Color Scheme

The plugin applies the following colors (using CLion's default color scheme):

| Element | Example | Color |
|---------|---------|-------|
| **Keywords** | `func`, `var`, `if`, `else`, `for`, `return`, `struct` | Purple/Blue (Bold) |
| **Types** | `int`, `string`, `float`, `void`, `char`, `byte`, `bit`, `double` | Teal/Cyan |
| **String Literals** | `"Hello World"` | Green |
| **Character Literals** | `'A'`, `'\n'` | Green |
| **Number Literals** | `42`, `3.14`, `1.5e10` | Blue |
| **Operators** | `+`, `-`, `*`, `/`, `==`, `!=`, `&&`, `||` | Dark Gray |
| **Punctuation** | `{`, `}`, `(`, `)`, `[`, `]`, `;` | Dark Gray |
| **Comments** | `// This is a comment` | Gray (Italic) |
| **Identifiers** | `myVariable`, `calculateSum` | Default Text Color |

## 📝 Example Code (Before Highlighting)

```
// Factorial function
func factorial(n:int) -> int {
    var result:int = 1;
    for (var i:int = 1; i <= n; i++) {
        var temp:int = result * i;
        result = temp;
    }
    return result;
}

func main() -> void {
    var name:string = "Alice";
    var age:int = 25;
    var pi:float = 3.14159;
    var flag:bit = 1;
    
    print("Hello, " + name + "!");
    print("Age: " + age);
    
    if (age >= 18) {
        print("Adult");
    } else if (age >= 13) {
        print("Teenager");
    } else {
        print("Child");
    }
    
    var fact10:int = factorial(10);
    print("10! = " + fact10);
}

struct Point {
    int x;
    int y;
    
    func move(dx:int, dy:int) -> void {
        x = x + dx;
        y = y + dy;
    }
    
    func getX() -> int {
        return x;
    }
}
```

## 🌈 After Highlighting (Annotated)

Here's how the code appears with syntax highlighting applied:

```
[GRAY ITALIC]// Factorial function[/GRAY ITALIC]
[PURPLE]func[/PURPLE] factorial(n[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL]) [DARK GRAY]->[/DARK GRAY] [TEAL]int[/TEAL] [DARK GRAY]{[/DARK GRAY]
    [PURPLE]var[/PURPLE] result[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL] [DARK GRAY]=[/DARK GRAY] [BLUE]1[/BLUE][DARK GRAY];[/DARK GRAY]
    [PURPLE]for[/PURPLE] [DARK GRAY]([/DARK GRAY][PURPLE]var[/PURPLE] i[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL] [DARK GRAY]=[/DARK GRAY] [BLUE]1[/BLUE][DARK GRAY];[/DARK GRAY] i [DARK GRAY]<=[/DARK GRAY] n[DARK GRAY];[/DARK GRAY] i[DARK GRAY]++)[/DARK GRAY] [DARK GRAY]{[/DARK GRAY]
        [PURPLE]var[/PURPLE] temp[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL] [DARK GRAY]=[/DARK GRAY] result [DARK GRAY]*[/DARK GRAY] i[DARK GRAY];[/DARK GRAY]
        result [DARK GRAY]=[/DARK GRAY] temp[DARK GRAY];[/DARK GRAY]
    [DARK GRAY]}[/DARK GRAY]
    [PURPLE]return[/PURPLE] result[DARK GRAY];[/DARK GRAY]
[DARK GRAY]}[/DARK GRAY]

[PURPLE]func[/PURPLE] main[DARK GRAY]()[/DARK GRAY] [DARK GRAY]->[/DARK GRAY] [TEAL]void[/TEAL] [DARK GRAY]{[/DARK GRAY]
    [PURPLE]var[/PURPLE] name[DARK GRAY]:[/DARK GRAY][TEAL]string[/TEAL] [DARK GRAY]=[/DARK GRAY] [GREEN]"Alice"[/GREEN][DARK GRAY];[/DARK GRAY]
    [PURPLE]var[/PURPLE] age[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL] [DARK GRAY]=[/DARK GRAY] [BLUE]25[/BLUE][DARK GRAY];[/DARK GRAY]
    [PURPLE]var[/PURPLE] pi[DARK GRAY]:[/DARK GRAY][TEAL]float[/TEAL] [DARK GRAY]=[/DARK GRAY] [BLUE]3.14159[/BLUE][DARK GRAY];[/DARK GRAY]
    
    print[DARK GRAY]([/DARK GRAY][GREEN]"Hello, "[/GREEN] [DARK GRAY]+[/DARK GRAY] name [DARK GRAY]+[/DARK GRAY] [GREEN]"!"[/GREEN][DARK GRAY]);[/DARK GRAY]
    
    [PURPLE]if[/PURPLE] [DARK GRAY]([/DARK GRAY]age [DARK GRAY]>=[/DARK GRAY] [BLUE]18[/BLUE][DARK GRAY])[/DARK GRAY] [DARK GRAY]{[/DARK GRAY]
        print[DARK GRAY]([/DARK GRAY][GREEN]"Adult"[/GREEN][DARK GRAY]);[/DARK GRAY]
    [DARK GRAY]}[/DARK GRAY] [PURPLE]else[/PURPLE] [PURPLE]if[/PURPLE] [DARK GRAY]([/DARK GRAY]age [DARK GRAY]>=[/DARK GRAY] [BLUE]13[/BLUE][DARK GRAY])[/DARK GRAY] [DARK GRAY]{[/DARK GRAY]
        print[DARK GRAY]([/DARK GRAY][GREEN]"Teenager"[/GREEN][DARK GRAY]);[/DARK GRAY]
    [DARK GRAY]}[/DARK GRAY] [PURPLE]else[/PURPLE] [DARK GRAY]{[/DARK GRAY]
        print[DARK GRAY]([/DARK GRAY][GREEN]"Child"[/GREEN][DARK GRAY]);[/DARK GRAY]
    [DARK GRAY]}[/DARK GRAY]
[DARK GRAY]}[/DARK GRAY]

[PURPLE]struct[/PURPLE] Point [DARK GRAY]{[/DARK GRAY]
    [TEAL]int[/TEAL] x[DARK GRAY];[/DARK GRAY]
    [TEAL]int[/TEAL] y[DARK GRAY];[/DARK GRAY]
    
    [PURPLE]func[/PURPLE] move[DARK GRAY]([/DARK GRAY]dx[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL][DARK GRAY],[/DARK GRAY] dy[DARK GRAY]:[/DARK GRAY][TEAL]int[/TEAL][DARK GRAY])[/DARK GRAY] [DARK GRAY]->[/DARK GRAY] [TEAL]void[/TEAL] [DARK GRAY]{[/DARK GRAY]
        x [DARK GRAY]=[/DARK GRAY] x [DARK GRAY]+[/DARK GRAY] dx[DARK GRAY];[/DARK GRAY]
        y [DARK GRAY]=[/DARK GRAY] y [DARK GRAY]+[/DARK GRAY] dy[DARK GRAY];[/DARK GRAY]
    [DARK GRAY]}[/DARK GRAY]
[DARK GRAY]}[/DARK GRAY]
```

## 📸 Visual Examples

### Example 1: Function Declaration
```javascript
func add(a:int, b:int) -> int {
    return a + b;
}
```
**Highlighted elements:**
- `func` → Purple (keyword)
- `int` → Teal (type)
- `return` → Purple (keyword)
- `a`, `b` → Default (identifiers)
- `+` → Dark Gray (operator)

### Example 2: Variable Declarations
```javascript
var name:string = "World";
var age:int = 25;
var pi:float = 3.14;
var flag:bit = 1;
```
**Highlighted elements:**
- `var` → Purple (keyword)
- `string`, `int`, `float`, `bit` → Teal (types)
- `"World"` → Green (string literal)
- `25`, `3.14`, `1` → Blue (numbers)

### Example 3: Control Structures
```javascript
if (x > 10) {
    print("Large");
} else if (x > 5) {
    print("Medium");
} else {
    print("Small");
}
```
**Highlighted elements:**
- `if`, `else` → Purple (keywords)
- `>` → Dark Gray (operator)
- `10`, `5` → Blue (numbers)
- `"Large"`, `"Medium"`, `"Small"` → Green (strings)

### Example 4: For Loop
```javascript
for (var i:int = 0; i < 10; i++) {
    print("Iteration: " + i);
}
```
**Highlighted elements:**
- `for`, `var` → Purple (keywords)
- `int` → Teal (type)
- `0`, `10` → Blue (numbers)
- `<`, `++` → Dark Gray (operators)
- `"Iteration: "` → Green (string)

### Example 5: Struct with Methods
```javascript
struct Rectangle {
    int width;
    int height;
    
    func area() -> int {
        return width * height;
    }
}
```
**Highlighted elements:**
- `struct`, `func`, `return` → Purple (keywords)
- `int` → Teal (type)
- `*` → Dark Gray (operator)
- `width`, `height`, `area` → Default (identifiers)

### Example 6: Comments
```javascript
// This is a single-line comment
var x:int = 42;  // Inline comment
```
**Highlighted elements:**
- `// This is a single-line comment` → Gray Italic (comment)
- `// Inline comment` → Gray Italic (comment)
- `var`, keywords → Purple
- `int` → Teal

### Example 7: All Data Types
```javascript
var a:int = 100;
var b:char = 'X';
var c:byte = 255;
var d:bit = 1;
var e:float = 3.14;
var f:double = 2.718;
var g:string = "Text";
```
**Highlighted elements:**
- `var` → Purple (repeated 7 times)
- `int`, `char`, `byte`, `bit`, `float`, `double`, `string` → Teal (all types)
- `100`, `255`, `1`, `3.14`, `2.718` → Blue (numbers)
- `'X'` → Green (character)
- `"Text"` → Green (string)

### Example 8: Complex Expression
```javascript
var result:int = (a + b) * (c - d) / e;
```
**Highlighted elements:**
- `var`, keywords → Purple
- `int` → Teal
- `+`, `-`, `*`, `/` → Dark Gray (operators)
- `(`, `)` → Dark Gray (punctuation)
- All variable names → Default

### Example 9: String Operations
```javascript
var firstName:string = "John";
var lastName:string = "Doe";
print("Full name: " + firstName + " " + lastName);
```
**Highlighted elements:**
- `"John"`, `"Doe"`, `"Full name: "`, `" "` → Green (strings)
- `+` → Dark Gray (operator)

### Example 10: Arrays
```javascript
var[10] numbers:int;
numbers[0] = 42;
var value:int = numbers[5];
```
**Highlighted elements:**
- `var` → Purple
- `int` → Teal
- `[`, `]` → Dark Gray (punctuation)
- `0`, `5`, `42` → Blue (numbers)

## 🎯 Key Features

1. **Consistent Coloring**: Same elements always have the same color
2. **Clear Hierarchy**: Keywords stand out, types are distinct, literals are obvious
3. **Readable Comments**: Gray italic makes comments easy to skip
4. **Operator Visibility**: Operators are subtle but clear
5. **String Recognition**: Easy to spot string and character literals

## 🔧 Customization

All colors can be customized in CLion:
1. **Settings** → **Editor** → **Color Scheme** → **Custom Compiler Language**
2. Select any element (Keyword, Type, String, etc.)
3. Modify foreground/background/font style
4. Click **Apply**

## 📝 Notes

- The actual colors depend on your CLion theme (Dark/Light)
- Default shown here is based on Darcula (dark theme)
- All colors map to IntelliJ's standard color scheme categories
- Colors are semantically appropriate (e.g., keywords are bold, comments are muted)

---

**Plugin Version:** 1.0.0  
**Compatible with:** CLion 2023.2+  
**Compiler Version:** 5.1.0
