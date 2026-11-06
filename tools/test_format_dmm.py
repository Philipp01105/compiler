import unittest

from format_dmm import format_source, tokenize


class RawStringFormattingTest(unittest.TestCase):
    def test_raw_string_remains_one_token(self):
        tokens = tokenize(r'var path = r"C:\new\tools\file.dmm";')

        self.assertEqual(
            [(token.kind, token.text) for token in tokens],
            [
                ("word", "var"),
                ("word", "path"),
                ("punctuation", "="),
                ("literal", r'r"C:\new\tools\file.dmm"'),
                ("punctuation", ";"),
            ],
        )

    def test_formatter_preserves_raw_string_contents(self):
        source = 'func main()->void{var value=r"\\n\\t\\q";}'

        self.assertEqual(
            format_source(source),
            'func main() -> void {\n    var value = r"\\n\\t\\q";\n}\n',
        )


if __name__ == "__main__":
    unittest.main()
