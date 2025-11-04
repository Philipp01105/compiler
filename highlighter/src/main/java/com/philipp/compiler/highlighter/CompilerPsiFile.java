package com.philipp.compiler.highlighter;

import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.psi.FileViewProvider;
import org.jetbrains.annotations.NotNull;

public class CompilerPsiFile extends PsiFileBase {
    public CompilerPsiFile(@NotNull FileViewProvider viewProvider) {
        super(viewProvider, CompilerLanguage.INSTANCE);
    }

    @NotNull
    @Override
    public FileType getFileType() {
        return CompilerFileType.INSTANCE;
    }

    @Override
    public String toString() {
        return "Compiler Language File";
    }
}
