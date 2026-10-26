"""An asynchronous, bounded JSON-RPC client for the local Codex app-server."""
import asyncio
import contextlib
import json
import os
from pathlib import Path
import signal


class BackendError(RuntimeError):
    pass


def member_instructions(name, partner):
    display = 'GPT' if name == 'gpt' else 'DeepSeek'
    partner_display = 'GPT' if partner == 'gpt' else 'DeepSeek'
    return f'''你是 {display}，正在一个共享对话中；组员是 {partner_display}，用户也在场。
像普通 Codex 一样自然回答、讨论或完成任务。用户输入不会被改写成任务模板。
所有公开发言都会显示给用户和组员；不要冒充组员发言。
需要决定下一棒时，在回复最后单独一行写隐藏标记：
[[next:{partner}]]
[[next:user]]
[[end]]
这些标记不会显示给用户。没有标记时，终端按默认顺序接力。'''


class CodexBackend:
    def __init__(self, name, command, env, cwd, emit, approval=None, timeout=900, overrides=None, partner=None):
        self.name, self.command, self.env = name, command, env
        self.cwd, self.emit, self.approval = str(Path(cwd).resolve()), emit, approval
        self.timeout = timeout
        self.overrides = dict(overrides or {})
        self.partner = partner or ('deepseek' if name == 'gpt' else 'gpt')
        self.process = None
        self.thread_id = self.turn_id = None
        self._rpc_id = 0
        self._pending = {}
        self._turn_done = None
        self._final_text = ''
        self._writable = False
        self._workspace = self.cwd
        self._tasks = set()
        self._lock = asyncio.Lock()
        self._closed = False
        self._stderr_tail = []
        self._items = {}
        self._secrets = [v for k, v in env.items() if any(s in k.upper() for s in ('KEY', 'TOKEN', 'SECRET')) and len(v) >= 8]

    def clean(self, value):
        text = str(value)
        for secret in self._secrets:
            text = text.replace(secret, '[已隐藏凭据]')
        return text

    def _task(self, coroutine):
        task = asyncio.create_task(coroutine)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        return task

    async def start(self):
        if self.process and self.process.returncode is None:
            return False
        self._closed = False
        self.process = await asyncio.create_subprocess_exec(
            *self.command, cwd=self.cwd, env=self.env,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE, start_new_session=True, limit=4 * 1024 * 1024)
        self._task(self._read())
        self._task(self._stderr())
        try:
            await self.rpc('initialize', {'clientInfo': {'name': 'codex_team', 'title': 'Team', 'version': '0.1.0'}})
            await self.send({'method': 'initialized', 'params': {}})
            parameters = {
                'cwd': self.cwd, 'sandbox': 'read-only', 'approvalPolicy': 'on-request',
                'approvalsReviewer': 'user',
                'developerInstructions': member_instructions(self.name, self.partner),
                'config': {**self.overrides, 'agents.enabled': False, 'skills.include_instructions': False},
            }
            if self.overrides.get('model'):
                parameters['model'] = self.overrides['model']
            if self.overrides.get('model_provider'):
                parameters['modelProvider'] = self.overrides['model_provider']
            result = await self.rpc('thread/start', parameters)
            expected = self.overrides.get('model_provider')
            if expected and result['thread'].get('modelProvider') != expected:
                raise BackendError('后台没有使用指定的模型提供商，停止以避免把两个成员接到同一个模型。')
            self.thread_id = result['thread']['id']
            return True
        except BaseException:
            await self.close()
            raise

    async def send(self, message):
        if self.process is None or self.process.returncode is not None:
            raise BackendError(f'{self.name} 后台未运行')
        self.process.stdin.write((json.dumps(message, ensure_ascii=False) + '\n').encode())
        await self.process.stdin.drain()

    async def rpc(self, method, params):
        self._rpc_id += 1
        request_id = self._rpc_id
        future = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        try:
            await self.send({'id': request_id, 'method': method, 'params': params})
            return await asyncio.wait_for(future, min(self.timeout, 60))
        finally:
            self._pending.pop(request_id, None)

    async def _stderr(self):
        while line := await self.process.stderr.readline():
            self._stderr_tail.append(self.clean(line.decode(errors='replace').strip()))
            self._stderr_tail = self._stderr_tail[-8:]

    async def _read(self):
        try:
            while line := await self.process.stdout.readline():
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                if 'method' in message and 'id' in message:
                    self._task(self._server_request(message))
                elif 'id' in message:
                    future = self._pending.get(message['id'])
                    if future and not future.done():
                        if 'error' in message:
                            future.set_exception(BackendError(self.clean(message['error'].get('message', message['error']))))
                        else:
                            future.set_result(message.get('result', {}))
                else:
                    self._event(message.get('method'), message.get('params', {}))
        except (OSError, ValueError) as error:
            self.emit(self.name, 'error', self.clean(error))
        finally:
            if not self._closed:
                error = BackendError(f'{self.name} 后台已退出。' + '\n'.join(self._stderr_tail))
                for future in [*self._pending.values(), self._turn_done]:
                    if future and not future.done():
                        future.set_exception(error)

    async def _server_request(self, message):
        method, params = message['method'], message.get('params', {})
        try:
            if method in ('item/commandExecution/requestApproval', 'item/fileChange/requestApproval'):
                decision = 'decline'
                if self._writable and self._workspace == self.cwd and self.approval:
                    details = dict(params, teamItem=self._items.get(params.get('itemId'), {}))
                    decision = await self.approval(self.name, method, details)
                else:
                    self.emit(self.name, 'status', '当前回合未获得该写入权限，已拒绝权限提升。')
                await self.send({'id': message['id'], 'result': {'decision': decision}})
            else:
                await self.send({'id': message['id'], 'error': {'code': -32601, 'message': '此协作入口不支持该交互，请在公开回复中说明阻碍。'}})
        except (OSError, BackendError):
            pass

    def _event(self, method, params):
        if params.get('threadId') not in (None, self.thread_id):
            return
        if method == 'turn/started' and self._turn_done:
            self.turn_id = params['turn']['id']
        elif method in ('item/started', 'item/completed') and self._turn_done and not self._turn_done.done():
            if params.get('turnId') != self.turn_id:
                return
            item = params.get('item', {})
            if item.get('id'):
                self._items[item['id']] = item
            if item.get('type') == 'agentMessage' and method == 'item/completed':
                text = item.get('text', '')
                if item.get('phase') in ('final_answer', None):
                    self._final_text = text
                elif text:
                    self.emit(self.name, 'progress', self.clean(text))
            elif item.get('type') == 'commandExecution' and method == 'item/started':
                self.emit(self.name, 'command', self.clean(item.get('command', '正在运行命令')))
            elif item.get('type') == 'fileChange' and method == 'item/completed':
                self.emit(self.name, 'files', ', '.join(self.clean(x.get('path', '')) for x in item.get('changes', [])))
        elif method == 'turn/completed' and self._turn_done and not self._turn_done.done():
            turn = params['turn']
            if turn['id'] != self.turn_id:
                return
            if turn['status'] == 'completed':
                self._turn_done.set_result(self._final_text)
            else:
                detail = (turn.get('error') or {}).get('message', turn['status'])
                self._turn_done.set_exception(BackendError(self.clean(detail)))
        elif method == 'error' and params.get('message'):
            self.emit(self.name, 'error', self.clean(params['message']))

    async def turn(self, prompt, writable=False, workspace=None):
        async with self._lock:
            if self._closed:
                raise BackendError('后台已关闭，请重新启动 team')
            self._writable = writable
            path = Path(workspace or self.cwd).resolve()
            if not path.is_dir() or not path.is_relative_to(Path(self.cwd)):
                raise BackendError('子任务工作范围不在项目内，或目录不存在。')
            self._workspace = str(path)
            self._final_text = ''
            self._items = {}
            self._turn_done = asyncio.get_running_loop().create_future()
            self.turn_id = None
            sandbox = {'type': 'workspaceWrite', 'writableRoots': [self._workspace], 'networkAccess': False, 'excludeSlashTmp': True, 'excludeTmpdirEnvVar': True} if writable else {'type': 'readOnly', 'networkAccess': False}
            try:
                result = await self.rpc('turn/start', {
                    'threadId': self.thread_id, 'input': [{'type': 'text', 'text': prompt}],
                    'cwd': self._workspace,
                    'sandboxPolicy': sandbox, 'approvalPolicy': 'on-request',
                    'approvalsReviewer': 'user',
                })
                self.turn_id = result['turn']['id']
                raw = await asyncio.wait_for(asyncio.shield(self._turn_done), self.timeout)
                return self.clean(raw)
            except (asyncio.CancelledError, asyncio.TimeoutError):
                await self.interrupt()
                raise
            finally:
                if self._turn_done and self._turn_done.done() and not self._turn_done.cancelled():
                    self._turn_done.exception()  # Consume an interrupted future on cancellation.
                self._turn_done = None
                self.turn_id = None
                self._writable = False

    async def steer(self, text):
        if not self.turn_id or not self._turn_done or self._turn_done.done():
            return False
        try:
            await self.rpc('turn/steer', {'threadId': self.thread_id, 'expectedTurnId': self.turn_id,
                                        'input': [{'type': 'text', 'text': text}]})
            return True
        except BackendError:
            return False

    async def interrupt(self):
        done = self._turn_done
        if done and not done.done() and not self.turn_id:
            # turn/start can be accepted upstream before its response supplies the ID.
            # Without that ID there is no safe interrupt request; reap the process.
            await self.close()
            return
        if self.turn_id and done and not done.done():
            try:
                await asyncio.wait_for(self.rpc('turn/interrupt', {'threadId': self.thread_id, 'turnId': self.turn_id}), 5)
                with contextlib.suppress(BackendError):
                    await asyncio.wait_for(asyncio.shield(done), 5)
            except (BackendError, asyncio.TimeoutError, OSError):
                await self.close()

    async def close(self):
        if self._closed:
            return
        self._closed = True
        if self.process and self.process.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGTERM)
            try:
                await asyncio.wait_for(self.process.wait(), 2)
            except asyncio.TimeoutError:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(self.process.pid, signal.SIGKILL)
                await self.process.wait()
        for future in [*self._pending.values(), self._turn_done]:
            if future and not future.done():
                future.set_exception(BackendError(f'{self.name} 后台已关闭'))
        tasks = list(self._tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
