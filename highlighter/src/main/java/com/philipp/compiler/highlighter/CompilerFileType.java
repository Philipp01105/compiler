package com.philipp.compiler.highlighter;

import com.intellij.openapi.fileTypes.LanguageFileType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.*;

public class CompilerFileType extends LanguageFileType {
    public static final CompilerFileType INSTANCE = new CompilerFileType();

    private CompilerFileType() {
        super(CompilerLanguage.INSTANCE);
    }

    @NotNull
    @Override
    public String getName() {
        return "Compiler Language File";
    }

    @NotNull
    @Override
    public String getDescription() {
        return "Custom compiler language file";
    }

    @NotNull
    @Override
    public String getDefaultExtension() {
        return "txt";
    }

    @Nullable
    @Override
    public Icon getIcon() {
        return null; // Can add custom icon later
    }
}
