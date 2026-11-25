"""Property-based tests (Hypothesis) for pure parts of the CLI: the system
tests' console reader (any split of QEMU's output into chunks reads the
same) and the compile_commands merge (one entry per file, idempotent).

A failing example is printed with what reproduces it; Hypothesis keeps it
in .hypothesis/ and tries it first next time."""

import json
import tempfile
import unittest
from pathlib import Path

try:
    from hypothesis import given, settings
    from hypothesis import strategies as st
except ImportError:  # an older dev image: requirements.txt has it, CI installs it
    raise unittest.SkipTest(
        "hypothesis is not installed (pip install -r requirements.txt)"
    )

from aember.steps.testing.system.vm import Console
from aember.utils.merge_compile_command import CompileCommandsMerger

# Pieces of what a serial console prints: text, line ends, colours, cursor
# moves, an OSC title, escapes we don't know.
PIECES = st.sampled_from(
    [
        "a",
        "Z",
        " ",
        "#",
        "\n",
        "\r\n",
        "\r",
        "\x1b[0m",
        "\x1b[32m",
        "\x1b[1;31m",
        "\x1b[2J",
        "\x1b[?25l",
        "\x1b]0;title\x07",
        "\x1bc",
        "\x1bM",
        "\x1bZ",
        "é",
    ]
)


def read(chunks: list[str]) -> str:
    console = Console()
    for chunk in chunks:
        console.add(chunk)
    return console.text


class ConsoleProperty(unittest.TestCase):
    @given(pieces=st.lists(PIECES, max_size=40), cuts=st.sets(st.integers(0, 400)))
    @settings(max_examples=300)
    def test_any_split_into_chunks_reads_the_same(self, pieces, cuts):
        text = "".join(pieces)
        points = sorted({0, len(text), *[c for c in cuts if c <= len(text)]})
        chunks = [text[a:b] for a, b in zip(points, points[1:])]
        self.assertEqual(read(chunks), read([text]))

    @given(pieces=st.lists(PIECES, max_size=40))
    def test_no_escape_sequence_or_carriage_return_is_left(self, pieces):
        text = read(["".join(pieces)])
        self.assertNotIn("\r", text)
        for known in ("\x1b[", "\x1b]", "\x1bc", "\x1bM"):
            self.assertNotIn(known, text)
        # What is left is the text and the line ends.
        self.assertEqual(text.count("\n"), sum(p.count("\n") for p in pieces))


ENTRY = st.fixed_dictionaries(
    {
        "directory": st.sampled_from(["/b/libs", "/b/services", "/b/exe"]),
        "file": st.sampled_from(["a.cpp", "b.cpp", "c.cpp", "d.cpp"]),
        "command": st.text(max_size=8),
    }
)


class CompileCommandsProperty(unittest.TestCase):
    @given(files=st.lists(st.lists(ENTRY, max_size=6), max_size=4))
    def test_one_entry_per_file_the_last_wins_idempotent(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            sources = []
            for i, entries in enumerate(files):
                p = Path(tmp) / f"{i}.json"
                p.write_text(json.dumps(entries))
                sources.append(p)
            merged = CompileCommandsMerger._merge_entries(sources)
            keys = [(e["directory"], e["file"]) for e in merged]
            self.assertEqual(len(keys), len(set(keys)))
            last = {}
            for entries in files:
                for e in entries:
                    last[(e["directory"], e["file"])] = e
            self.assertEqual({k: e for k, e in zip(keys, merged)}, last)
            again = Path(tmp) / "merged.json"
            again.write_text(json.dumps(merged))
            self.assertEqual(CompileCommandsMerger._merge_entries([again]), merged)
            self.assertEqual(
                CompileCommandsMerger._merge_entries(sources + [again]), merged
            )


if __name__ == "__main__":
    unittest.main()
