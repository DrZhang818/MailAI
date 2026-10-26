import asyncio
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import Team, parse_handoff


class Member:
    def __init__(self, name, responses, calls):
        self.name, self.responses, self.calls = name, list(responses), calls
        self.active = False
        self.gate = None
        self.steered = []

    async def start(self):
        return True

    async def turn(self, prompt, writable=False, workspace=None):
        self.calls.append((self.name, prompt, writable, workspace))
        self.active = True
        try:
            if self.gate:
                await self.gate.wait()
            await asyncio.sleep(0)
            if not self.responses:
                raise AssertionError('unexpected turn: ' + prompt[:120])
            result = self.responses.pop(0)
            if isinstance(result, Exception):
                raise result
            return result
        finally:
            self.active = False

    async def steer(self, text):
        if self.active:
            self.steered.append(text)
            return True
        return False

    async def interrupt(self):
        pass

    async def close(self):
        pass


class NaturalRuntimeTest(unittest.IsolatedAsyncioTestCase):
    def team(self, gpt, deepseek, **kwargs):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.calls, self.events = [], []
        self.members = {
            name: Member(name, responses, self.calls)
            for name, responses in [('gpt', gpt), ('deepseek', deepseek)]
        }
        team = Team(self.members, lambda *args: self.events.append(args), cwd=self.root, **kwargs)
        self.addAsyncCleanup(team.close)
        return team

    async def until(self, predicate):
        for _ in range(300):
            if predicate():
                return
            await asyncio.sleep(.001)
        self.fail('expected state was not reached')

    async def test_first_user_turn_is_verbatim_and_default_relay_returns_to_user(self):
        team = self.team(['能收到。', '目前只是通信确认。'], ['我也能收到。'])
        await team.submit('能收到我的消息吗')
        result = await team.wait()
        self.assertTrue(result['ok'])
        self.assertEqual([name for name, _, _, _ in self.calls], ['gpt', 'deepseek', 'gpt'])
        self.assertEqual(self.calls[0][1], '能收到我的消息吗')
        self.assertTrue(all(writable for _, _, writable, _ in self.calls))
        self.assertIn('[你]', self.calls[1][1])
        self.assertIn('[GPT]', self.calls[1][1])
        self.assertIn('我也能收到', self.calls[2][1])
        self.assertNotIn('阶段：', self.calls[1][1])
        self.assertNotIn('当前任务', self.calls[2][1])

    async def test_explicit_marker_is_hidden_and_extends_relay_beyond_default_sequence(self):
        team = self.team(
            ['A\n[[next:deepseek]]', 'C\n[[next:deepseek]]'],
            ['B\n[[next:gpt]]', 'D\n[[next:user]]'],
        )
        await team.submit('讨论一下')
        result = await team.wait()
        self.assertTrue(result['ok'])
        self.assertEqual([name for name, _, _, _ in self.calls], ['gpt', 'deepseek', 'gpt', 'deepseek'])
        visible = [text for source, kind, text in self.events
                   if source in ('gpt', 'deepseek') and kind == 'message']
        self.assertEqual(visible, ['A', 'B', 'C', 'D'])
        self.assertFalse(any('[[next:' in text for text in visible))

    async def test_user_and_end_markers_return_the_floor_immediately(self):
        team = self.team(['完成。\n[[next:user]]'], [])
        await team.submit('回答后结束')
        self.assertTrue((await team.wait())['ok'])
        self.assertEqual(len(self.calls), 1)

        team = self.team(['完成。\n[[end]]'], [])
        self.calls.clear()
        await team.submit('再试一次')
        self.assertTrue((await team.wait())['ok'])
        self.assertEqual(len(self.calls), 1)

    async def test_user_message_during_active_turn_steers_without_starting_another_task(self):
        team = self.team(['收到补充。\n[[next:user]]'], [])
        self.members['gpt'].gate = asyncio.Event()
        await team.submit('原消息')
        await self.until(lambda: bool(self.calls))
        await team.submit('补充消息')
        await self.until(lambda: bool(self.members['gpt'].steered))
        self.members['gpt'].gate.set()
        self.assertTrue((await team.wait())['ok'])
        self.assertEqual(len(self.calls), 1)
        self.assertIn('补充消息', self.members['gpt'].steered[0])
        self.assertEqual([record['text'] for record in team.room if record['source'] == 'user'], ['原消息', '补充消息'])

    async def test_marker_can_sustain_more_than_ten_turns_and_end_only_on_model_request(self):
        gpt = [f'G{i}\n[[next:deepseek]]' for i in range(6)]
        deepseek = [f'D{i}\n[[next:gpt]]' for i in range(5)] + ['最后。\n[[next:user]]']
        team = self.team(gpt, deepseek)
        await team.submit('长讨论')
        self.assertTrue((await team.wait())['ok'])
        self.assertEqual(len(self.calls), 12)

    async def test_repeated_no_progress_relay_stops_instead_of_looping_forever(self):
        team = self.team(
            ['同样的话\n[[next:deepseek]]'] * 4,
            ['同样的话\n[[next:gpt]]'] * 4,
            max_loop_repeats=2,
        )
        await team.submit('开始')
        result = await team.wait()
        self.assertFalse(result['ok'])
        self.assertIn('重复', result['reason'])
        self.assertLessEqual(len(self.calls), 6)

    async def test_parse_handoff_rejects_unknown_or_internal_markers(self):
        self.assertEqual(parse_handoff('正文\n[[next:deepseek]]'), ('正文', 'deepseek'))
        self.assertEqual(parse_handoff('正文\n[[end]]'), ('正文', 'end'))
        self.assertEqual(parse_handoff('正文\n[[next:other]]'), ('正文\n[[next:other]]', None))

