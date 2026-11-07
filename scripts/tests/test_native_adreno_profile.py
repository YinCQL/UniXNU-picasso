import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('profile', Path(__file__).resolve().parents[1] / 'analyze-native-adreno-profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def report(count, total, ibpeek=None, omit=None):
    lines = [f'PICASSO_GPU prof {p} n={count} us={total} max=10'
             for p in profile.PHASES if p != omit]
    if ibpeek is not None:
        lines += [f'PICASSO_GPU prof ibpeek n={count} us={ibpeek} max=9']
    lines += [f'PICASSO_GPU profscan pub={count}/0/{count}/{count * 4096} acq=0/0/0/0',
              f'PICASSO_GPU profmaint pub={count}/{count * 4}K acq=0/0K']
    return '\n'.join(lines) + '\n'


class AnalysisTests(unittest.TestCase):
    def test_old_reports_remain_readable(self):
        d = profile.analyze(report(10, 100) + report(14, 300))
        self.assertEqual(len(d['samples']), 2)
        self.assertEqual(d['intervals'][0]['calls'], 4)
        self.assertEqual(d['intervals'][0]['phases']['body']['us_per_submit'], 50)
        self.assertNotIn('ibpeek', d['intervals'][0]['phases'])

    def test_ibpeek_and_maintenance_deltas(self):
        d = profile.analyze(report(10, 100, 80) + report(14, 300, 92))['intervals'][0]
        self.assertEqual(d['phases']['ibpeek']['us_per_submit'], 3)
        self.assertEqual(d['scan']['pub'], [4, 0, 4, 16384])
        self.assertEqual(d['maintenance']['pub'], [4, 16])

    def test_incomplete_report_is_not_sample(self):
        d = profile.analyze(report(10, 100, omit='ring') + report(14, 300))
        self.assertEqual(len(d['samples']), 1)
        self.assertEqual(d['intervals'], [])

    def test_idle_and_counter_reset_are_not_intervals(self):
        d = profile.analyze(report(10, 100) * 2 + report(1, 10))
        self.assertEqual(d['intervals'], [])

    def test_display_counter_has_separate_rate(self):
        a = 'eagl f600 i1\nPICASSO_DISPLAY frame 30 time_ms=1000\n' + report(10, 100)
        b = 'eagl f620 i1\nPICASSO_DISPLAY frame 60 time_ms=1500\n' + report(14, 300)
        d = profile.analyze(a + b)['intervals'][0]
        self.assertEqual(d['frame_delta'], 20)
        self.assertEqual(d['dpu_flip_rate'], 60)


if __name__ == '__main__':
    unittest.main()
