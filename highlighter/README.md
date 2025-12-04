# Custom Compiler Language - CLion Syntax Highlighter Plugin

This is a CLion-compatible plugin that provides full syntax highlighting support for the custom compiler language.

## Features

- ✅ **Comprehensive Syntax Highlighting** for all language constructs
- ✅ **Keyword Recognition**: `func`, `var`, `if`, `else`, `for`, `return`, `struct`
- ✅ **Type Highlighting**: `int`, `char`, `byte`, `bit`, `float`, `double`, `string`, `void`
- ✅ **String and Character Literals**: Full support with escape sequences
- ✅ **Number Literals**: Integer and floating-point numbers
- ✅ **Operators**: All arithmetic, comparison, and logical operators
- ✅ **Comments**: Line comments (`//`)
- ✅ **Customizable Colors**: Configure colors through CLion's settings

## Language Features Supported

The plugin highlights the following language features:

### Data Types (7 types)
- `int` - 32-bit signed integer
- `char` - 8-bit character
- `byte` - 8-bit unsigned integer
- `bit` - Boolean (0 or 1)
- `float` - 32-bit floating point
- `double` - 64-bit floating point
- `string` - String pointer type

### Keywords
- `func` - Function declaration
- `var` - Variable declaration
- `if`, `else` - Conditional statements
- `for` - Loop statement
- `return` - Return statement
- `struct` - Structure declaration

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`
- Logical: `&&`, `||`, `!`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`
- Increment/Decrement: `++`, `--`
- Member access: `.`

## Building the Plugin

### Prerequisites

- **Java JDK 17 or later**
- **Gradle 7.0 or later**
- **CLion 2023.2 or later** (for testing)

### Build Steps

1. Navigate to the `highlighter` directory:
   ```bash
   cd highlighter
   ```

2. Build the plugin using Gradle:
   ```bash
   ./gradlew buildPlugin
   ```
   
   On Windows:
   ```cmd
   gradlew.bat buildPlugin
   ```

3. The built plugin will be located at:
   ```
   highlighter/build/distributions/compiler-highlighter-1.0.0.zip
   ```

## Installing the Plugin in CLion

### Method 1: Install from Disk

1. Open CLion
2. Go to **File** → **Settings** (on Windows/Linux) or **CLion** → **Preferences** (on macOS)
3. Navigate to **Plugins**
4. Click the **⚙️ (gear icon)** → **Install Plugin from Disk...**
5. Select the built plugin ZIP file: `highlighter/build/distributions/compiler-highlighter-1.0.0.zip`
6. Click **OK** and restart CLion

### Method 2: Install from Source (Development)

1. Build the plugin using `./gradlew buildPlugin`
2. In CLion, go to **File** → **Settings** → **Plugins**
3. Click **⚙️ (gear icon)** → **Install Plugin from Disk...**
4. Navigate to and select the ZIP file from `build/distributions/`
5. Restart CLion when prompted

## Using the Plugin

Once installed, the plugin will automatically recognize and highlight `.txt` files containing the custom compiler language syntax.

### Testing the Highlighter

1. Open any `.txt` file in your compiler project (e.g., `test.txt`)
2. The syntax should be automatically highlighted with:
   - **Keywords** in bold purple/blue
   - **Types** in teal/cyan
   - **Strings** in green
   - **Numbers** in blue
   - **Comments** in gray/italic
   - **Operators** in standard operator colors

### Customizing Colors

1. Go to **File** → **Settings** → **Editor** → **Color Scheme** → **Custom Compiler Language**
2. Customize the colors for each syntax element:
   - Keyword
   - Type
   - String
   - Character
   - Number
   - Operator
   - Punctuation
   - Comment
   - Identifier

## Example Code

Here's what highlighted code looks like:

```javascript
// Calculate factorial
func factorial(n:int) -> int {
    var result:int = 1;
    for (var i:int = 1; i <= n; i++) {
        var temp:int = result * i;
        result = temp;
    }
    return result;
}

// Main function
func main() -> void {
    var name:string = "World";
    var age:int = 25;
    var pi:float = 3.14;
    
    print("Hello, " + name + "!");
    print("Age: " + age);
    
    if (age >= 18) {
        print("Adult");
    } else {
        print("Child");
    }
    
    var fact10:int = factorial(10);
    print("10! = " + fact10);
}

// Struct with methods
struct Point {
    int x;
    int y;
    
    func move(dx:int, dy:int) -> void {
        x = x + dx;
        y = y + dy;
    }
}
```

## Development

### Project Structure

```
highlighter/
├── build.gradle.kts                 # Gradle build configuration
├── settings.gradle.kts              # Gradle settings
├── README.md                        # This file
├── INSTALL.md                       # Installation guide
└── src/main/
    ├── java/com/philipp/compiler/highlighter/
    │   ├── CompilerLanguage.java              # Language definition
    │   ├── CompilerFileType.java              # File type registration
    │   ├── CompilerTokenTypes.java            # Token type definitions
    │   ├── CompilerLexer.java                 # Lexical analyzer
    │   ├── CompilerParser.java                # Simple parser
    │   ├── CompilerParserDefinition.java      # Parser configuration
    │   ├── CompilerPsiElement.java            # PSI element wrapper
    │   ├── CompilerPsiFile.java               # PSI file representation
    │   ├── CompilerSyntaxHighlighter.java     # Syntax highlighter implementation
    │   ├── CompilerSyntaxHighlighterFactory.java  # Highlighter factory
    │   └── CompilerColorSettingsPage.java     # Color settings page
    └── resources/
        └── META-INF/
            └── plugin.xml                     # Plugin configuration
```

### Running in Development Mode

To test the plugin without installing:

```bash
./gradlew runIde
```

This will launch a new instance of CLion with the plugin loaded.

### Making Changes

1. Modify the source files in `src/main/java/`
2. Rebuild the plugin: `./gradlew buildPlugin`
3. Reinstall in CLion or run `./gradlew runIde` to test

## Troubleshooting

### Plugin doesn't load
- Check CLion version compatibility (2023.2+)
- Verify Java 17+ is installed
- Check the plugin build completed without errors

### Syntax not highlighting
- Ensure the file has a `.txt` extension
- Try closing and reopening the file
- Check **File** → **Settings** → **Editor** → **File Types** to verify the association

### Build fails
- Ensure Gradle 7.0+ is installed
- Check internet connection (Gradle needs to download dependencies)
- Run `./gradlew clean` and try again

### Colors look wrong
- Go to **Settings** → **Editor** → **Color Scheme** → **Custom Compiler Language**
- Reset to defaults or customize as needed

## Version History

### Version 1.0.0 (2025-11-04)
- Initial release
- Full syntax highlighting support
- Support for all 7 data types
- Keyword, operator, and literal recognition
- Comment support
- Customizable color scheme
- Compatible with CLion 2023.2+

## License

This plugin is part of the compiler project by Philipp01105.

## Support

For issues, questions, or contributions:
- GitHub: https://github.com/Philipp01105/compiler
- Create an issue on the GitHub repository

## Contributing

Contributions are welcome! To contribute:
1. Fork the repository
2. Make your changes in the `highlighter/` directory
3. Test the plugin with `./gradlew runIde`
4. Submit a pull request

## Credits

**Author:** Philipp01105  
**Date:** 2025-11-04  
**Version:** 1.0.0  
**Compiler Version:** 5.1.0

This plugin provides syntax highlighting for the custom compiler language with support for:
- 7 data types
- Functions with parameters
- Control structures
- Operators
- String and character literals
- Comments
- Structs with methods
