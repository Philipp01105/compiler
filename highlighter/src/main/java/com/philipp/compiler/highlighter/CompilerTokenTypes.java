package com.philipp.compiler.highlighter;

import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NonNls;
import org.jetbrains.annotations.NotNull;

public class CompilerTokenTypes {
    public static final IElementType KEYWORD = new CompilerTokenType("KEYWORD");
    public static final IElementType TYPE = new CompilerTokenType("TYPE");
    public static final IElementType IDENTIFIER = new CompilerTokenType("IDENTIFIER");
    public static final IElementType STRING_LITERAL = new CompilerTokenType("STRING_LITERAL");
    public static final IElementType CHAR_LITERAL = new CompilerTokenType("CHAR_LITERAL");
    public static final IElementType NUMBER = new CompilerTokenType("NUMBER");
    public static final IElementType OPERATOR = new CompilerTokenType("OPERATOR");
    public static final IElementType PUNCTUATION = new CompilerTokenType("PUNCTUATION");
    public static final IElementType COMMENT = new CompilerTokenType("COMMENT");
    public static final IElementType WHITE_SPACE = new CompilerTokenType("WHITE_SPACE");
    public static final IElementType BAD_CHARACTER = new CompilerTokenType("BAD_CHARACTER");

    public static class CompilerTokenType extends IElementType {
        public CompilerTokenType(@NotNull @NonNls String debugName) {
            super(debugName, CompilerLanguage.INSTANCE);
        }
    }
}
