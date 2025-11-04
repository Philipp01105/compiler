package com.philipp.compiler.highlighter;

import com.intellij.lang.ASTNode;
import com.intellij.lang.ParserDefinition;
import com.intellij.lang.PsiParser;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.project.Project;
import com.intellij.psi.FileViewProvider;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IFileElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;

public class CompilerParserDefinition implements ParserDefinition {
    public static final IFileElementType FILE = new IFileElementType(CompilerLanguage.INSTANCE);

    @NotNull
    @Override
    public Lexer createLexer(Project project) {
        return new CompilerLexer();
    }

    @Override
    public @NotNull PsiParser createParser(Project project) {
        return new CompilerParser();
    }

    @Override
    public @NotNull IFileElementType getFileNodeType() {
        return FILE;
    }

    @NotNull
    @Override
    public TokenSet getWhitespaceTokens() {
        return TokenSet.create(CompilerTokenTypes.WHITE_SPACE);
    }

    @NotNull
    @Override
    public TokenSet getCommentTokens() {
        return TokenSet.create(CompilerTokenTypes.COMMENT);
    }

    @NotNull
    @Override
    public TokenSet getStringLiteralElements() {
        return TokenSet.create(CompilerTokenTypes.STRING_LITERAL, CompilerTokenTypes.CHAR_LITERAL);
    }

    @NotNull
    @Override
    public PsiElement createElement(ASTNode node) {
        return new CompilerPsiElement(node);
    }

    @Override
    public @NotNull PsiFile createFile(@NotNull FileViewProvider viewProvider) {
        return new CompilerPsiFile(viewProvider);
    }
}
