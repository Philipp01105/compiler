package com.philipp.compiler.highlighter;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

import static com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey;

public class CompilerSyntaxHighlighter extends SyntaxHighlighterBase {
    public static final TextAttributesKey KEYWORD = 
        createTextAttributesKey("COMPILER_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD);
    
    public static final TextAttributesKey TYPE = 
        createTextAttributesKey("COMPILER_TYPE", DefaultLanguageHighlighterColors.CLASS_NAME);
    
    public static final TextAttributesKey STRING = 
        createTextAttributesKey("COMPILER_STRING", DefaultLanguageHighlighterColors.STRING);
    
    public static final TextAttributesKey CHAR = 
        createTextAttributesKey("COMPILER_CHAR", DefaultLanguageHighlighterColors.STRING);
    
    public static final TextAttributesKey NUMBER = 
        createTextAttributesKey("COMPILER_NUMBER", DefaultLanguageHighlighterColors.NUMBER);
    
    public static final TextAttributesKey OPERATOR = 
        createTextAttributesKey("COMPILER_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN);
    
    public static final TextAttributesKey PUNCTUATION = 
        createTextAttributesKey("COMPILER_PUNCTUATION", DefaultLanguageHighlighterColors.BRACES);
    
    public static final TextAttributesKey COMMENT = 
        createTextAttributesKey("COMPILER_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT);
    
    public static final TextAttributesKey IDENTIFIER = 
        createTextAttributesKey("COMPILER_IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER);
    
    public static final TextAttributesKey BAD_CHARACTER = 
        createTextAttributesKey("COMPILER_BAD_CHARACTER", DefaultLanguageHighlighterColors.INVALID_STRING_ESCAPE);

    private static final TextAttributesKey[] KEYWORD_KEYS = new TextAttributesKey[]{KEYWORD};
    private static final TextAttributesKey[] TYPE_KEYS = new TextAttributesKey[]{TYPE};
    private static final TextAttributesKey[] STRING_KEYS = new TextAttributesKey[]{STRING};
    private static final TextAttributesKey[] CHAR_KEYS = new TextAttributesKey[]{CHAR};
    private static final TextAttributesKey[] NUMBER_KEYS = new TextAttributesKey[]{NUMBER};
    private static final TextAttributesKey[] OPERATOR_KEYS = new TextAttributesKey[]{OPERATOR};
    private static final TextAttributesKey[] PUNCTUATION_KEYS = new TextAttributesKey[]{PUNCTUATION};
    private static final TextAttributesKey[] COMMENT_KEYS = new TextAttributesKey[]{COMMENT};
    private static final TextAttributesKey[] IDENTIFIER_KEYS = new TextAttributesKey[]{IDENTIFIER};
    private static final TextAttributesKey[] BAD_CHAR_KEYS = new TextAttributesKey[]{BAD_CHARACTER};
    private static final TextAttributesKey[] EMPTY_KEYS = new TextAttributesKey[0];

    @NotNull
    @Override
    public Lexer getHighlightingLexer() {
        return new CompilerLexer();
    }

    @NotNull
    @Override
    public TextAttributesKey[] getTokenHighlights(IElementType tokenType) {
        if (tokenType.equals(CompilerTokenTypes.KEYWORD)) {
            return KEYWORD_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.TYPE)) {
            return TYPE_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.STRING_LITERAL)) {
            return STRING_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.CHAR_LITERAL)) {
            return CHAR_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.NUMBER)) {
            return NUMBER_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.OPERATOR)) {
            return OPERATOR_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.PUNCTUATION)) {
            return PUNCTUATION_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.COMMENT)) {
            return COMMENT_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.IDENTIFIER)) {
            return IDENTIFIER_KEYS;
        } else if (tokenType.equals(CompilerTokenTypes.BAD_CHARACTER)) {
            return BAD_CHAR_KEYS;
        } else {
            return EMPTY_KEYS;
        }
    }
}
