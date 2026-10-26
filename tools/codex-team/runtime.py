"""A natural two-model conversation with a hidden floor-control marker."""
import asyncio
from collections import deque
from pathlib import Path
import re

LABELS = {'gpt': 'GPT', 'deepseek': 'DeepSeek', 'team': '小组', 'user': '你'}

_MARKER = re.compile(
    r'(?mi)^[ \t]*\[\[(?:next:(gpt|deepseek|user)|(end))\]\][ \t]*$'
)


def parse_handoff(text):
    """Return visible text and the requested next speaker, if any."""
    text = str(text or '')
    found = list(_MARKER.finditer(text))
    if not found:
        return text.strip(), None
    last = found[-1]
    target = 'end' if last.group(2) else last.group(1).lower()
    return _MARKER.sub('', text).strip(), target


class Team:
    def __init__(self, members, emit, read_only=False, cwd=None, default_order=None,
                 max_loop_repeats=3):
        self.members, self.emit = members, emit
        self.cwd = Path(cwd or Path.cwd()).resolve()
        self.read_only = read_only
        self.default_order = list(default_order or ['gpt', 'deepseek', 'gpt'])
        self.default_order = [name for name in self.default_order if name in members] or list(members)[:1]
        self.max_loop_repeats = max(1, int(max_loop_repeats))
        self.room = []
        self.seen = {name: set() for name in members}
        self.states = {name: '待命' for name in members}
        self.phase = '待命'
        self.job = self.last_result = None
        self.exchange_number = 0
        self._active_name = None
        self._generation = 0
        self._deliveries = set()
        self._history = deque(maxlen=max(2, self.max_loop_repeats * 2))

    @property
    def running(self):
        return self.job is not None and not self.job.done()

    def publish(self, source, text, kind='message'):
        record = {
            'seq': len(self.room) + 1,
            'source': source,
            'text': text,
            'kind': kind,
            'exchange': self.exchange_number,
        }
        self.room.append(record)
        self.emit(source, kind, text)
        return record

    def member_event(self, source, kind, text):
        if kind == 'progress':
            self.publish(source, text, kind)
        else:
            self.emit(source, kind, text)

    async def _deliver(self, record, generation):
        if generation != self._generation or not self._active_name:
            return
        member = self.members[self._active_name]
        try:
            delivered = await member.steer(f'[用户]\n{record["text"]}')
        except Exception:
            return
        if delivered and generation == self._generation:
            self.seen[self._active_name].add(record['seq'])

    async def submit(self, text):
        text = text.strip()
        if not text:
            return
        record = self.publish('user', text)
        if self.running:
            delivery = asyncio.create_task(self._deliver(record, self._generation))
            self._deliveries.add(delivery)
            delivery.add_done_callback(self._deliveries.discard)
            await asyncio.sleep(0)
            return
        self.exchange_number += 1
        self._generation += 1
        self.phase = '对话中'
        self._history.clear()
        self.job = asyncio.create_task(self._run(self._generation))

    def _unread_prompt(self, name):
        unread = [
            record for record in self.room
            if record['seq'] not in self.seen[name] and record['source'] != name
        ]
        if not unread:
            return None
        self.seen[name].update(record['seq'] for record in unread)
        if len(unread) == 1 and unread[0]['source'] == 'user':
            return unread[0]['text']
        return '\n\n'.join(
            f'[{LABELS.get(record["source"], record["source"])}]\n{record["text"]}'
            for record in unread
        )

    async def _ask(self, name):
        prompt = self._unread_prompt(name)
        if prompt is None:
            return None
        self.states[name] = '发言中'
        self._active_name = name
        try:
            response = await self.members[name].turn(prompt, writable=not self.read_only)
        except Exception:
            self.states[name] = '请求失败'
            raise
        finally:
            self._active_name = None
        self.states[name] = '待命'
        visible, target = parse_handoff(response)
        if visible:
            self.publish(name, visible)
        return visible, target

    def _is_repeated_loop(self):
        if len(self._history) < self.max_loop_repeats * 2:
            return False
        window = list(self._history)[-self.max_loop_repeats * 2:]
        if len({item[1].casefold() for item in window}) != 1:
            return False
        counts = {}
        for item in window:
            key = (item[0], item[1].casefold(), item[2])
            counts[key] = counts.get(key, 0) + 1
        return any(count >= self.max_loop_repeats for count in counts.values())

    async def _parallel(self, coroutines):
        tasks = [asyncio.create_task(coroutine) for coroutine in coroutines]
        try:
            return await asyncio.gather(*tasks)
        except BaseException:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            raise

    async def _start(self, name, member):
        self.states[name] = '连接中'
        try:
            if await member.start():
                self.seen[name].clear()
            self.states[name] = '待命'
        except Exception:
            self.states[name] = '连接失败'
            raise

    async def _run(self, generation):
        try:
            await self._parallel([
                self._start(name, member) for name, member in self.members.items()
            ])
            position = 0
            speaker = self.default_order[position]
            turns = 0
            while generation == self._generation:
                result = await self._ask(speaker)
                if result is None:
                    break
                visible, target = result
                turns += 1
                self._history.append((speaker, visible or '', target))
                if self._is_repeated_loop():
                    raise RuntimeError('检测到重复接力，已停止并交还用户。')
                if target in ('user', 'end'):
                    break
                if target in self.members and target != speaker:
                    speaker = target
                    later = [i for i in range(position + 1, len(self.default_order))
                             if self.default_order[i] == speaker]
                    if later:
                        position = later[0]
                    continue
                position += 1
                if position >= len(self.default_order):
                    break
                speaker = self.default_order[position]
            self.phase = '待命'
            self.last_result = {
                'ok': True,
                'turns': turns,
                'conversation': list(self.room),
            }
            return self.last_result
        except asyncio.CancelledError:
            self.phase = '已停止'
            self.last_result = {'ok': False, 'reason': '已停止'}
            return self.last_result
        except Exception as error:
            self.phase = '未完成'
            self.last_result = {'ok': False, 'reason': str(error), 'conversation': list(self.room)}
            self.emit('team', 'error', str(error))
            return self.last_result
        finally:
            self._active_name = None

    async def wait(self):
        return await asyncio.shield(self.job) if self.job else self.last_result

    async def stop(self):
        self._generation += 1
        running = self.running
        deliveries = list(self._deliveries)
        for delivery in deliveries:
            delivery.cancel()
        await asyncio.gather(*deliveries, return_exceptions=True)
        if running:
            self.job.cancel()
            await asyncio.gather(self.job, return_exceptions=True)
            self.publish('team', '已停止；可以继续输入。', 'cancellation')
        await asyncio.gather(
            *(member.interrupt() for member in self.members.values()),
            return_exceptions=True,
        )

    def status(self):
        active = f'；当前：{LABELS[self._active_name]}' if self._active_name else ''
        states = ' · '.join(f'{LABELS[name]}：{state}' for name, state in self.states.items())
        return f'对话 #{self.exchange_number} · {self.phase}{active}\n{states}'

    async def close(self):
        await self.stop()
        await asyncio.gather(
            *(member.close() for member in self.members.values()),
            return_exceptions=True,
        )

