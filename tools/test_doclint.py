#!/usr/bin/env python3
# ABOUTME: Tests for tools/doclint.py: history wording in reference docs and finished items in TODO.md are caught in English and
# ABOUTME: in Portuguese (upstream text), and ordinary present-tense English is not. Run: python tools/test_doclint.py
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import doclint  # noqa: E402


def history(line):
    return [k for k, _ in doclint.problemas_referencia(line)]


def finished(text):
    return [msg for _, msg in doclint.checa_todo(text) if "finished item" in msg]


class ReferenceHistory(unittest.TestCase):
    def test_english_history_is_caught(self):
        for line in ("The crash was fixed in passing while porting the HUD.",
                     "The camera bug was fixed along the way.",
                     "Fixed in 1a2b3c4 by moving the call.",
                     "Done in commit abcdef0.",
                     "It used to reset the timer every round.",
                     "This is already on master.",
                     "Verified by the owner in a 4-player fight.",
                     "From a report by the owner: the shadow doubles."):
            with self.subTest(line=line):
                self.assertTrue(history(line), line)

    def test_present_tense_english_is_not_caught(self):
        for line in ("The table is used to draw the HUD.",
                     "These values are used to pick the stage.",
                     "The game continues when the button used to exit is released.",
                     "A PNG of the snapshot's VRAM; used to compare with the native texture.",
                     "The stage's long name (same index; confirmed by the owner).",
                     "The keyboard bindings stay fixed in the user's profile.",
                     "A fixed step of two fields per frame; fixed on screen.",
                     "The split can be done in the hook instead.",
                     "The turret no longer follows the enemy (retail does).",
                     "Translate it in a separate commit right after the merge.",
                     'No "it used to" wording in reference docs.'):
            with self.subTest(line=line):
                self.assertEqual(history(line), [], line)

    def test_portuguese_history_is_still_caught(self):
        self.assertTrue(history("O bug foi corrigido de passagem."))
        self.assertTrue(history("Antes era assim, depois do merge mudou."))

    def test_date_outside_a_path_is_caught(self):
        self.assertEqual(history("Measured on 2026-10-18."), ["2026-10-18"])
        self.assertEqual(history("See `docs/investigacoes/2026-10-18/x.md`."), [])


class TodoFinished(unittest.TestCase):
    def test_english_finished_items_are_caught(self):
        for text in ("- **Shadow bug** fixed in 1a2b3c4",
                     "- Camera drift (done in abcdef0)",
                     "- ~~Old item~~",
                     "- **Done:** the HUD",
                     "- Lens flare: verified by the owner",
                     "- Mods menu, now on master"):
            with self.subTest(text=text):
                self.assertTrue(finished(text), text)

    def test_open_english_items_are_not_caught(self):
        for text in ("- **Mod file larger than the disc's**: the game loads into fixed-size regions. Make room per file type.",
                     "- **4 players at 60:** the split can be done in the hook; candidates are fixed in the profile.",):
            with self.subTest(text=text):
                self.assertEqual(finished(text), [], text)

    def test_portuguese_finished_items_are_still_caught(self):
        self.assertTrue(finished("- Sombra dupla (feito em 1a2b3c4)"))


if __name__ == "__main__":
    unittest.main()
