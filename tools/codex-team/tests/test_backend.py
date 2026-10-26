import asyncio
import sys
import unittest
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from backend import CodexBackend, BackendError


class BackendTest(unittest.IsolatedAsyncioTestCase):
    async def make_backend(self, scenario='normal', approval=None):
        events = []
        backend = CodexBackend('gpt', [sys.executable, str(Path(__file__).with_name('fake_app_server.py')), scenario], {}, Path('/tmp'), lambda *args: events.append(args), approval=approval, timeout=3)
        self.addAsyncCleanup(backend.close)
        await backend.start()
        return backend, events

    async def test_subprocess_round_trip_returns_plain_text_and_streams_commands(self):
        backend, events = await self.make_backend()
        result = await backend.turn('hello')
        self.assertEqual(result, 'readOnly:hello')
        self.assertTrue(any(event[1] == 'command' and 'pwd' in event[2] for event in events))
        result = await backend.turn('execute', writable=True)
        self.assertEqual(result, 'workspaceWrite:execute')

    async def test_user_prompt_is_sent_without_a_phase_or_task_wrapper(self):
        backend, _ = await self.make_backend()
        self.assertEqual(await backend.turn('能收到我的消息吗'), 'readOnly:能收到我的消息吗')

    async def test_readonly_agent_cannot_escalate_even_if_ui_would_accept(self):
        async def accept(*args):
            self.fail('read-only approval must never reach the UI')
        backend, _ = await self.make_backend('approval', accept)
        result = await backend.turn('review')
        self.assertEqual(result, 'decline')

    async def test_writable_approval_is_forwarded(self):
        async def approve(name, method, params):
            self.assertEqual(name, 'gpt')
            self.assertEqual(params['command'], 'touch file')
            return 'accept'
        backend, _ = await self.make_backend('approval', approve)
        self.assertEqual(await backend.turn('execute', writable=True), 'accept')

    async def test_provider_error_is_reported_as_failure(self):
        backend, _ = await self.make_backend('failed-turn')
        with self.assertRaisesRegex(BackendError, 'provider failed'):
            await backend.turn('hello')

    async def test_interrupt_finishes_pending_turn_and_close_reaps_process(self):
        backend, _ = await self.make_backend('hang')
        task = asyncio.create_task(backend.turn('wait'))
        for _ in range(100):
            if backend.turn_id:
                break
            await asyncio.sleep(.01)
        await backend.interrupt()
        with self.assertRaises((BackendError, asyncio.CancelledError)):
            await task
        process = backend.process
        await backend.close()
        self.assertIsNotNone(process.returncode)

    async def test_cancel_before_turn_id_arrives_closes_the_unknown_active_turn(self):
        backend, _ = await self.make_backend('delayed-start')
        task = asyncio.create_task(backend.turn('execute', writable=True))
        await asyncio.sleep(.05)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertIsNotNone(backend.process.returncode, 'unknown writable turn must not survive cancellation')

    async def test_startup_failure_reaps_process(self):
        backend = CodexBackend('gpt', [sys.executable, str(Path(__file__).with_name('fake_app_server.py')), 'startup-error'], {}, Path('/tmp'), lambda *args: None, timeout=3)
        self.addAsyncCleanup(backend.close)
        with self.assertRaisesRegex(BackendError, 'cannot start'):
            await backend.start()
        self.assertIsNotNone(backend.process.returncode)

    async def test_profile_is_loaded_through_thread_config_and_provider_is_explicit(self):
        overrides = {'model': 'fixture-deepseek', 'model_provider': 'deepseek-menu',
                     'model_providers': {'deepseek-menu': {'wire_api': 'responses', 'env_key': 'DEEPSEEK_API_KEY'}}}
        backend = CodexBackend('deepseek', [sys.executable, str(Path(__file__).with_name('fake_app_server.py')), 'provider-config'], {}, Path('/tmp'), lambda *args: None, overrides=overrides, timeout=3)
        self.addAsyncCleanup(backend.close)
        await backend.start()
        self.assertEqual(await backend.turn('hello'), 'provider-ok')

    async def test_thread_uses_only_minimal_room_identity_and_handoff_protocol(self):
        backend, _ = await self.make_backend('instructions')
        instructions = await backend.turn('show instructions')
        self.assertIn('你是 GPT', instructions)
        self.assertIn('DeepSeek', instructions)
        self.assertIn('[[next:deepseek]]', instructions)
        self.assertNotIn('任务 #', instructions)
        self.assertNotIn('阶段：', instructions)
        self.assertNotIn('唯一执行者', instructions)

    async def test_scoped_turn_pins_cwd_and_writable_roots_to_assigned_directory(self):
        backend, _ = await self.make_backend('scope')
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            result = await backend.turn('scoped', writable=True, workspace=Path(directory))
            self.assertEqual(result, directory + '::' + directory)
        with self.assertRaisesRegex(BackendError, '范围'):
            await backend.turn('outside', writable=True, workspace=Path('/etc'))

    async def test_scoped_writer_cannot_escalate_past_its_assignment(self):
        async def accept(*args):
            self.fail('scoped worker cannot gain unrestricted approval while peers run')
        backend, _ = await self.make_backend('approval', accept)
        with tempfile.TemporaryDirectory(dir='/tmp') as directory:
            result = await backend.turn('scoped', writable=True, workspace=Path(directory))
            self.assertEqual(result, 'decline')
