# Unicode 17.0.0

Import "stdlib/unicode" for Category, combiningClass, property predicates, locale-independent mapCase and normalization. category(scalar) returns a descriptive Category enum. collectCodepoints/fromCodepoints own sequences; iterate(&bytes).next() returns Result<Option<u32>,Error> and retains the checked source borrow. Errors carry kind/offset and include allocation failure. Byte APIs preserve embedded NUL.

Small strict decode/encode/validate operations belong to stdlib/text/utf8 and stdlib/text/utf16, independent of Unicode tables. They reject overlong UTF-8, invalid scalars, truncation and malformed surrogate pairs, and check output capacity before writes.

mapCase(input,Lower/Upper/Title/Fold) supports expanding mappings and contextual final sigma. Title maps individual scalars rather than words; Turkic/Lithuanian locale rules and collation are excluded. normalize(input,NFC/NFD/NFKC/NFKD) performs recursive decomposition, canonical ordering/composition and algorithmic Hangul.

Data/license are in data/. Run python stdlib/unicode/generate.py from the repository root to reproduce tables.dmm, SHA256SUMS and conformance vectors; --check validates without writing. The generator uses checked-in Unicode data independently of Python's UCD. Tests execute official normalization rows and invariants, full casing mappings and scalar UTF-8 round trips at O0/O1.
