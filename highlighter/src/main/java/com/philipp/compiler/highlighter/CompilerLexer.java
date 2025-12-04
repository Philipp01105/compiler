package com.philipp.compiler.highlighter;

import com.intellij.lexer.LexerBase;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

public class CompilerLexer extends LexerBase {
    private CharSequence buffer;
    private int startOffset;
    private int endOffset;
    private int currentOffset;
    private IElementType currentTokenType;
    private int currentTokenStart;
    private int currentTokenEnd;

    @Override
    public void start(@NotNull CharSequence buffer, int startOffset, int endOffset, int initialState) {
        this.buffer = buffer;
        this.startOffset = startOffset;
        this.endOffset = endOffset;
        this.currentOffset = startOffset;
        advance();
    }

    @Override
    public int getState() {
        return 0;
    }

    @Nullable
    @Override
    public IElementType getTokenType() {
        return currentTokenType;
    }

    @Override
    public int getTokenStart() {
        return currentTokenStart;
    }

    @Override
    public int getTokenEnd() {
        return currentTokenEnd;
    }

    @Override
    public void advance() {
        if (currentOffset >= endOffset) {
            currentTokenType = null;
            return;
        }

        currentTokenStart = currentOffset;
        char c = buffer.charAt(currentOffset);

        // Skip whitespace
        if (Character.isWhitespace(c)) {
            while (currentOffset < endOffset && Character.isWhitespace(buffer.charAt(currentOffset))) {
                currentOffset++;
            }
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.WHITE_SPACE;
            return;
        }

        // Comments (// style)
        if (c == '/' && currentOffset + 1 < endOffset && buffer.charAt(currentOffset + 1) == '/') {
            currentOffset += 2;
            while (currentOffset < endOffset && buffer.charAt(currentOffset) != '\n') {
                currentOffset++;
            }
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.COMMENT;
            return;
        }

        // String literals
        if (c == '"') {
            currentOffset++;
            while (currentOffset < endOffset) {
                char ch = buffer.charAt(currentOffset);
                if (ch == '"') {
                    currentOffset++;
                    break;
                }
                if (ch == '\\' && currentOffset + 1 < endOffset) {
                    currentOffset += 2;
                } else {
                    currentOffset++;
                }
            }
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.STRING_LITERAL;
            return;
        }

        // Character literals
        if (c == '\'') {
            currentOffset++;
            while (currentOffset < endOffset) {
                char ch = buffer.charAt(currentOffset);
                if (ch == '\'') {
                    currentOffset++;
                    break;
                }
                if (ch == '\\' && currentOffset + 1 < endOffset) {
                    currentOffset += 2;
                } else {
                    currentOffset++;
                }
            }
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.CHAR_LITERAL;
            return;
        }

        // Numbers
        if (Character.isDigit(c)) {
            while (currentOffset < endOffset && (Character.isDigit(buffer.charAt(currentOffset)) || buffer.charAt(currentOffset) == '.')) {
                currentOffset++;
            }
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.NUMBER;
            return;
        }

        // Identifiers and keywords
        if (Character.isLetter(c) || c == '_') {
            while (currentOffset < endOffset) {
                char ch = buffer.charAt(currentOffset);
                if (Character.isLetterOrDigit(ch) || ch == '_') {
                    currentOffset++;
                } else {
                    break;
                }
            }
            currentTokenEnd = currentOffset;
            String text = buffer.subSequence(currentTokenStart, currentTokenEnd).toString();
            currentTokenType = getKeywordOrIdentifier(text);
            return;
        }

        // Operators and punctuation
        if (isOperatorChar(c)) {
            currentOffset++;
            // Handle multi-character operators
            if (currentOffset < endOffset) {
                char next = buffer.charAt(currentOffset);
                if ((c == '=' && next == '=') ||
                    (c == '!' && next == '=') ||
                    (c == '<' && next == '=') ||
                    (c == '>' && next == '=') ||
                    (c == '&' && next == '&') ||
                    (c == '|' && next == '|') ||
                    (c == '+' && (next == '=' || next == '+')) ||
                    (c == '-' && (next == '=' || next == '-' || next == '>')) ||
                    (c == '*' && next == '=') ||
                    (c == '/' && next == '=')) {
                    currentOffset++;
                }
            }
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.OPERATOR;
            return;
        }

        // Punctuation
        if (c == '{' || c == '}' || c == '(' || c == ')' || c == '[' || c == ']' || 
            c == ';' || c == ',' || c == '.' || c == ':') {
            currentOffset++;
            currentTokenEnd = currentOffset;
            currentTokenType = CompilerTokenTypes.PUNCTUATION;
            return;
        }

        // Unknown character
        currentOffset++;
        currentTokenEnd = currentOffset;
        currentTokenType = CompilerTokenTypes.BAD_CHARACTER;
    }

    private boolean isOperatorChar(char c) {
        return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
               c == '=' || c == '<' || c == '>' || c == '!' ||
               c == '&' || c == '|';
    }

    private IElementType getKeywordOrIdentifier(String text) {
        // Keywords
        switch (text) {
            case "func":
            case "var":
            case "if":
            case "else":
            case "for":
            case "return":
            case "struct":
                return CompilerTokenTypes.KEYWORD;
            
            // Data types
            case "int":
            case "char":
            case "byte":
            case "bit":
            case "float":
            case "double":
            case "string":
            case "void":
                return CompilerTokenTypes.TYPE;
            
            default:
                return CompilerTokenTypes.IDENTIFIER;
        }
    }

    @NotNull
    @Override
    public CharSequence getBufferSequence() {
        return buffer;
    }

    @Override
    public int getBufferEnd() {
        return endOffset;
    }
}
