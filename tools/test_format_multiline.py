import unittest

from format_dmm import format_source, tokenize


class MultilineStringFormattingTest(unittest.TestCase):
    def test_multiline_string_remains_one_token(self):
        source = 'var message = """first\n    second\nthird""";'
        tokens = tokenize(source)

        self.assertEqual(
            [(token.kind, token.text) for token in tokens],
            [
                ("word", "var"),
                ("word", "message"),
                ("punctuation", "="),
                ("literal", '"""first\n    second\nthird"""'),
                ("punctuation", ";"),
            ],
        )

    def test_formatter_preserves_contents(self):
        source = 'func main()->void{var message="""first\n  second\nthird""";}'

        self.assertEqual(
            format_source(source),
            'func main() -> void {\n    var message = """first\n  second\nthird""";\n}\n',
        )


if __name__ == "__main__":
    unittest.main()
