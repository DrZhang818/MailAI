from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallTest(unittest.TestCase):
    def install(self, home, *args):
        return subprocess.run([sys.executable, str(ROOT / 'install.py'), '--home', str(home), *args], capture_output=True, text=True)

    def test_install_is_repeatable_and_launcher_runs_without_shell_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            (home / '.bashrc').write_text('keep shell config\n')
            for _ in range(2):
                result = self.install(home)
                self.assertEqual(result.returncode, 0, result.stderr)
            launcher = home / '.local/bin/team'
            result = subprocess.run([str(launcher), '--help'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('自然接力', result.stdout)
            self.assertEqual((home / '.bashrc').read_text(), 'keep shell config\n')
            sessions = home / '.local/share/codex-team/sessions'
            sessions.mkdir()
            (sessions / 'keep.jsonl').write_text('keep\n')
            result = self.install(home, '--uninstall')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(launcher.exists())
            self.assertEqual((sessions / 'keep.jsonl').read_text(), 'keep\n')

    def test_install_refuses_to_overwrite_another_team_command(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            (home / '.local/bin').mkdir(parents=True)
            existing = home / '.local/bin/team'
            existing.write_text('another tool\n')
            result = self.install(home)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(existing.read_text(), 'another tool\n')
