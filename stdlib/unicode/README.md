# Unicode 17.0.0

Import "stdlib/unicode". Strict decodeUtf8/encodeUtf8 and validate reject
overlong encodings, surrogates, truncation and values above U+10FFFF.
Errors carry a kind and byte/unit offset. encodeUtf8 checks capacity before writes.
codepoints/fromCodepoints convert owned sequences; the move-only iterate(input).next returns
Result<Option<u32>,UnicodeError>. fromUtf16/toUtf16 validate surrogate pairs.
All byte APIs preserve embedded NULs.

categoryCode returns the index into:
Cn Lu Ll Lt Lm Lo Mn Mc Me Nd Nl No Pc Pd Ps Pe Pi Pf Po Sm Sc Sk So Zs Zl Zp Cc Cf Cs Co.
combiningClass and isAlphabetic/isWhitespace/isDigit/isUpper/isLower/isLetter/
isMark/isNumber/isPunctuation/isSymbol/isControl use pinned UCD properties.

mapCase(input,Lower/Upper/Title/Fold) provides full locale-independent mappings,
including expanding mappings and contextual final sigma. Title maps individual
scalars, not words. Turkic and Lithuanian locale rules and collation are excluded.
normalize(input,NFC/NFD/NFKC/NFKD) performs recursive decomposition, stable
canonical ordering and composition, including algorithmic Hangul.

Data and its license live in data/. Run python stdlib/unicode/generate.py from
the repository root to reproduce tables.dmm, SHA256SUMS and binary test vectors.
Pass --check to verify those artifacts without writing them.
The generator reads checked-in Unicode data and does not depend on Python's UCD.
The conformance test executes every official NormalizationTest row and all twenty
normalization invariants at O0/O1, every mapped scalar's four case mappings,
and UTF-8 round trips for every valid scalar. Source:
[Unicode 17](https://www.unicode.org/versions/Unicode17.0.0/).
