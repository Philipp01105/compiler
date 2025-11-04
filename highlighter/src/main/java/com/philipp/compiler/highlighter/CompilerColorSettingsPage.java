package com.philipp.compiler.highlighter;

import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import com.intellij.openapi.options.colors.AttributesDescriptor;
import com.intellij.openapi.options.colors.ColorDescriptor;
import com.intellij.openapi.options.colors.ColorSettingsPage;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.*;
import java.util.Map;

public class CompilerColorSettingsPage implements ColorSettingsPage {
    private static final AttributesDescriptor[] DESCRIPTORS = new AttributesDescriptor[]{
        new AttributesDescriptor("Keyword", CompilerSyntaxHighlighter.KEYWORD),
        new AttributesDescriptor("Type", CompilerSyntaxHighlighter.TYPE),
        new AttributesDescriptor("String", CompilerSyntaxHighlighter.STRING),
        new AttributesDescriptor("Character", CompilerSyntaxHighlighter.CHAR),
        new AttributesDescriptor("Number", CompilerSyntaxHighlighter.NUMBER),
        new AttributesDescriptor("Operator", CompilerSyntaxHighlighter.OPERATOR),
        new AttributesDescriptor("Punctuation", CompilerSyntaxHighlighter.PUNCTUATION),
        new AttributesDescriptor("Comment", CompilerSyntaxHighlighter.COMMENT),
        new AttributesDescriptor("Identifier", CompilerSyntaxHighlighter.IDENTIFIER),
    };

    @Nullable
    @Override
    public Icon getIcon() {
        return null;
    }

    @NotNull
    @Override
    public SyntaxHighlighter getHighlighter() {
        return new CompilerSyntaxHighlighter();
    }

    @NotNull
    @Override
    public String getDemoText() {
        return """
                // This is a comment
                func main() -> void {
                    var name:string = "World";
                    var age:int = 25;
                    var grade:char = 'A';
                    var flag:bit = 1;
                    var pi:float = 3.14;
                    var precise:double = 2.718281828;
                    var count:byte = 255;
                    
                    print("Hello, " + name + "!");
                    print("Age: " + age);
                    
                    if (age >= 18) {
                        print("Adult");
                    } else if (age >= 13) {
                        print("Teenager");
                    } else {
                        print("Child");
                    }
                    
                    for (var i:int = 0; i < 10; i++) {
                        print("Iteration: " + i);
                    }
                }
                
                func add(a:int, b:int) -> int {
                    return a + b;
                }
                
                struct Point {
                    int x;
                    int y;
                    
                    func move(dx:int, dy:int) -> void {
                        x = x + dx;
                        y = y + dy;
                    }
                }
                """;
    }

    @Nullable
    @Override
    public Map<String, TextAttributesKey> getAdditionalHighlightingTagToDescriptorMap() {
        return null;
    }

    @NotNull
    @Override
    public AttributesDescriptor[] getAttributeDescriptors() {
        return DESCRIPTORS;
    }

    @NotNull
    @Override
    public ColorDescriptor[] getColorDescriptors() {
        return ColorDescriptor.EMPTY_ARRAY;
    }

    @NotNull
    @Override
    public String getDisplayName() {
        return "Custom Compiler Language";
    }
}
