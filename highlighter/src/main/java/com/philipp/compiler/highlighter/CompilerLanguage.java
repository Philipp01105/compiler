package com.philipp.compiler.highlighter;

import com.intellij.lang.Language;

public class CompilerLanguage extends Language {
    public static final CompilerLanguage INSTANCE = new CompilerLanguage();

    private CompilerLanguage() {
        super("CompilerLanguage");
    }
}
