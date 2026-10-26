import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import time
import unittest
import asyncio
import contextlib
import io

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cli import Approvals
from terminal import Terminal


class CliTest(unittest.TestCase):
    def test_help_explains_natural_relay_without_starting_models(self):
        result = subprocess.run([sys.executable, str(ROOT / 'cli.py'), '--help'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('自然接力', result.stdout)
        self.assertIn('/verbose', result.stdout)
        self.assertIn('--doctor', result.stdout)

    def test_empty_pipe_exits_without_creating_session_or_starting_models(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(ROOT / 'cli.py'), '--session-dir', directory], input='', capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_doctor_does_not_claim_gpt_login_without_local_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)
            (config / 'deepseek-menu.config.toml').write_text('model = "fixture-deepseek"\n')
            environment = dict(os.environ, CODEX_HOME=directory, DEEPSEEK_API_KEY='fixture-secret')
            environment.pop('OPENAI_API_KEY', None)
            result = subprocess.run([sys.executable, str(ROOT / 'cli.py'), '--doctor'], env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('GPT 登录：未确认', result.stdout)
            self.assertNotIn('现有登录与 DeepSeek 凭据可供复用', result.stdout)

    def test_complete_cli_flow_with_real_protocol_processes_and_one_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'bin'
            binary.mkdir()
            fixture = ROOT / 'tests/fake_app_server.py'
            executable = binary / 'codex'
            executable.write_text(f'#!{sys.executable}\nimport runpy,sys\nsys.argv=[{str(fixture)!r},"team"]\nrunpy.run_path({str(fixture)!r},run_name="__main__")\n')
            executable.chmod(0o755)
            config = root / 'config'
            config.mkdir()
            (config / 'deepseek-menu.config.toml').write_text('model = "fixture"\n')
            project = root / 'project'
            project.mkdir()
            environment = dict(os.environ, CODEX_HOME=str(config), DEEPSEEK_API_KEY='fixture-secret', PATH=str(binary) + os.pathsep + os.environ['PATH'])
            result = subprocess.run([sys.executable, str(ROOT / 'cli.py'), '-C', str(project), '--session-dir', str(root / 'sessions'), '创建一个文件并检查'], env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertEqual((project / 'artifact.txt').read_text(), 'one executor\n')
            self.assertIn('GPT', result.stdout)
            self.assertIn('DeepSeek', result.stdout)
            self.assertIn('检查通过', result.stdout)
            self.assertNotIn('命令', result.stdout)
            self.assertNotIn('pwd', result.stdout)
            self.assertNotIn('[[next:', result.stdout)
            self.assertNotIn('fixture-secret', result.stdout + result.stderr)
            self.assertEqual(len(list((root / 'sessions').glob('*.jsonl'))), 1)

    def test_command_events_are_hidden_until_verbose_is_enabled(self):
        terminal = Terminal()
        quiet = io.StringIO()
        with contextlib.redirect_stdout(quiet):
            terminal.emit('gpt', 'command', 'pwd && ls')
        self.assertEqual(quiet.getvalue(), '')
        terminal.toggle_commands()
        loud = io.StringIO()
        with contextlib.redirect_stdout(loud):
            terminal.emit('gpt', 'command', 'pwd && ls')
        self.assertIn('pwd && ls', loud.getvalue())

    def test_terminal_accepts_chinese_editing_and_keeps_background_events_visible(self):
        master, slave = pty.openpty()
        script = """import asyncio,sys
sys.path.insert(0,sys.argv[1])
from terminal import Terminal
async def main():
    with Terminal() as terminal:
        terminal.emit('team','stage','READY')
        text = await terminal.read()
        terminal.emit('team','done','RECEIVED:'+text)
asyncio.run(main())
"""
        process = subprocess.Popen([sys.executable, '-c', script, str(ROOT)], stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        output = b''
        try:
            deadline = time.monotonic() + 5
            while b'READY' not in output and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try:
                        output += os.read(master, 65536)
                    except OSError:
                        break
            self.assertIn(b'READY', output)
            # Replace the final Chinese character using the real line editor.
            os.write(master, '任务错'.encode() + b'\x7f' + '好'.encode() + b'\r')
            while process.poll() is None and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try:
                        output += os.read(master, 65536)
                    except OSError:
                        break
            process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, output.decode(errors='replace'))
            self.assertIn('RECEIVED:任务好', output.decode(errors='replace'))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)


class ApprovalTest(unittest.IsolatedAsyncioTestCase):
    async def test_file_approval_shows_requested_root_reason_and_available_diff(self):
        events = []
        approvals = Approvals(lambda *args: events.append(args), True)
        pending = asyncio.create_task(approvals.request('gpt', 'item/fileChange/requestApproval', {
            'reason': '需要修改项目外的配置', 'grantRoot': '/outside/config', 'itemId': 'item-7',
            'teamItem': {'changes': [{'path': '/outside/config/settings.toml', 'diff': '+ enabled = true'}]},
        }))
        await asyncio.sleep(0)
        shown = '\n'.join(event[2] for event in events)
        approvals.cancel()
        await pending
        self.assertIn('/outside/config', shown)
        self.assertIn('需要修改项目外的配置', shown)
        self.assertIn('settings.toml', shown)
        self.assertIn('+ enabled = true', shown)
