"""Pure layout-contract tests for the opt-in real-OBS capture wrapper."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "capture_showcase", Path(__file__).with_name("capture-showcase.py")
)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class ComparisonTest(unittest.TestCase):
    def test_three_way_comparison_in_input_order(self):
        selected = ("clean", "phantom-drift", "spectral-melt")
        self.assertEqual(capture.comparison_presets(selected), selected)

    def test_original_two_way_comparison_is_preserved(self):
        self.assertEqual(
            capture.comparison_presets(("clean", "instrument", "time-ribbons")),
            ("clean", "instrument", "time-ribbons"),
        )

    def test_no_clean_reference_means_no_comparison(self):
        self.assertEqual(
            capture.comparison_presets(("phantom-drift", "spectral-melt")), ()
        )

    def test_only_reference_means_no_comparison(self):
        self.assertEqual(capture.comparison_presets(("clean",)), ())

    def test_many_presets_stay_bounded(self):
        selected = ("clean", "phantom-drift", "spectral-melt", "memory-melt")
        self.assertEqual(
            capture.comparison_presets(selected),
            ("clean", "phantom-drift", "spectral-melt"),
        )

    def test_spectral_fracture_can_be_captured(self):
        self.assertIn("spectral-fracture", capture.PRESETS)

    def test_comparison_width_bounds(self):
        self.assertEqual(capture.comparison_panel_size(640, 360, 3), (420, 236))
        self.assertEqual(capture.comparison_panel_size(256, 144, 2), (256, 144))


if __name__ == "__main__":
    unittest.main()
