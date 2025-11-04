package com.philipp.compiler.highlighter;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.lang.ASTNode;
import org.jetbrains.annotations.NotNull;

public class CompilerPsiElement extends ASTWrapperPsiElement {
    public CompilerPsiElement(@NotNull ASTNode node) {
        super(node);
    }
}
