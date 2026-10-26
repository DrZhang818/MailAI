import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from settings import load_settings


class SettingsTest(unittest.TestCase):
    def test_reuses_profiles_and_saved_secret_without_command_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            (home / '.codex').mkdir()
            (home / '.codex/config.toml').write_text('model = "existing-gpt"\n')
            (home / '.codex/deepseek-menu.config.toml').write_text('model = "existing-deepseek"\n')
            creds = home / '.local/share/codex-menu'
            creds.mkdir(parents=True)
            (creds / 'credentials.json').write_text(json.dumps({'api_key': 'saved-secret'}))
            settings = load_settings(home, {}, binary='codex')
            self.assertEqual([x.name for x in settings], ['gpt', 'deepseek'])
            self.assertEqual(settings[1].env['DEEPSEEK_API_KEY'], 'saved-secret')
            self.assertNotIn('saved-secret', ' '.join(settings[1].command))
            self.assertNotIn('--profile', settings[1].command)
            self.assertEqual(settings[1].overrides['model'], 'existing-deepseek')
            self.assertNotIn('DEEPSEEK_API_KEY', settings[0].env)
            self.assertEqual(settings[0].model, 'existing-gpt')

    def test_environment_secret_takes_precedence_and_missing_key_is_actionable(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            (home / '.codex').mkdir()
            (home / '.codex/deepseek-menu.config.toml').write_text('model = "deepseek"\n')
            settings = load_settings(home, {'DEEPSEEK_API_KEY': 'env-secret'})
            self.assertEqual(settings[1].env['DEEPSEEK_API_KEY'], 'env-secret')
            with self.assertRaisesRegex(ValueError, 'DEEPSEEK_API_KEY'):
                load_settings(home, {})
