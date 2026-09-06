#!/usr/bin/env python3
"""Focused argument tests for protected corpus selection."""

import argparse
import contextlib
import io
import os
import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import verify_qr_generalization as verifier  # noqa: E402


class RejectingParser:
    @staticmethod
    def error(message):
        raise ValueError(message)


def arguments(corpus, seed=None, confirmed=False):
    return argparse.Namespace(
        corpus=corpus,
        seed=seed,
        confirm_holdout=confirmed,
    )


class CorpusArgumentTest(unittest.TestCase):
    def setUp(self):
        self.parser = RejectingParser()
        self.no_confirmation = mock.patch.dict(
            os.environ,
            {verifier.HOLDOUT_CONFIRMATION_ENV: ""},
        )
        self.no_confirmation.start()
        self.addCleanup(self.no_confirmation.stop)

    def test_custom_holdout_seed_requires_confirmation(self):
        args = arguments("custom", verifier.HOLDOUT_SEED)

        with self.assertRaisesRegex(ValueError, "holdout requires"):
            verifier.resolve_corpus(self.parser, args)

    def test_custom_holdout_seed_with_flag_becomes_protected_holdout(self):
        args = arguments("custom", verifier.HOLDOUT_SEED, confirmed=True)

        verifier.resolve_corpus(self.parser, args)

        self.assertEqual(args.corpus, "holdout")
        message = verifier.generation_error_message(args, OSError("/secret/case.png"))
        self.assertNotIn("/secret/case.png", message)

    def test_custom_holdout_seed_accepts_confirmation_environment(self):
        args = arguments("custom", verifier.HOLDOUT_SEED)

        with mock.patch.dict(
            os.environ,
            {verifier.HOLDOUT_CONFIRMATION_ENV: "1"},
        ):
            verifier.resolve_corpus(self.parser, args)

        self.assertEqual(args.corpus, "holdout")

    def test_named_holdout_requires_confirmation(self):
        args = arguments("holdout")

        with self.assertRaisesRegex(ValueError, "holdout requires"):
            verifier.resolve_corpus(self.parser, args)

    def test_other_custom_seed_remains_custom(self):
        args = arguments("custom", verifier.HOLDOUT_SEED + 1)

        verifier.resolve_corpus(self.parser, args)

        self.assertEqual(args.corpus, "custom")
        self.assertEqual(args.seed, verifier.HOLDOUT_SEED + 1)


class ReportContextTest(unittest.TestCase):
    def test_report_uses_summary_and_context(self):
        args = argparse.Namespace(
            corpus="baseline",
            seed=verifier.BASELINE_SEED,
            positive_count=2,
            negative_count=1,
            minimum_positive_rate=0.5,
            average=1.0,
            p95=1.0,
            timeout=1.0,
        )
        summary = verifier.RunSummary(1, 0, (0.1, 0.2, 0.3), ())
        context = verifier.ReportContext(
            "configuration-hash",
            {"python": "p", "opencv": "c", "numpy": "n"},
        )
        stdout = io.StringIO()

        with contextlib.redirect_stdout(stdout):
            status = verifier.report_and_gate(args, summary, context)

        self.assertEqual(status, 0)
        self.assertIn("configuration sha256: configuration-hash", stdout.getvalue())
        self.assertIn("average: 0.200000s", stdout.getvalue())
        self.assertIn("p95: 0.300000s", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
