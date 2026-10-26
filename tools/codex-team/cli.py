#!/usr/bin/env python3
"""One terminal for a natural GPT/DeepSeek conversation."""
import argparse
import asyncio
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import uuid

from backend import CodexBackend
from runtime import Team, LABELS, parse_handoff
from settings import load_settings
from terminal import Terminal, safe_text

HELP = '''直接输入消息；GPT 与 DeepSeek 会自然接力，没有固定轮数。
/status  查看当前发言人
/stop    停止接力（Ctrl+C 也可）
/verbose 显示或隐藏工具命令（默认隐藏）
/allow N 批准第 N 项操作一次
/deny N  拒绝第 N 项操作
/help    显示帮助
/quit    停止任务并退出（空输入时 Ctrl+D 也可）
↑/↓ 历史记录 · ←/→ 编辑 · 支持中文和多行粘贴'''


class Journal:
    def __init__(self, directory):
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.path = directory / (datetime.now().strftime('%Y%m%d-%H%M%S-') + uuid.uuid4().hex[:8] + '.jsonl')
        fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        self.stream = os.fdopen(fd, 'w', encoding='utf-8')

    def record(self, source, kind, text):
        self.stream.write(json.dumps({'time': datetime.now().isoformat(timespec='seconds'), 'source': source, 'kind': kind, 'text': safe_text(text)}, ensure_ascii=False) + '\n')
        self.stream.flush()

    def close(self):
        self.stream.close()


class Approvals:
    def __init__(self, emit, interactive):
        self.emit, self.interactive = emit, interactive
        self.pending = {}
        self.number = 0

    async def request(self, name, method, params):
        if not self.interactive:
            self.emit(name, 'status', '非交互运行无法获取新审批；已拒绝这项操作。请在交互终端继续。')
            return 'decline'
        self.number += 1
        number = self.number
        future = asyncio.get_running_loop().create_future()
        self.pending[number] = future
        details = [f'操作需要确认 #{number}']
        for key, label in [('command', '命令'), ('reason', '原因'), ('cwd', '工作目录'), ('grantRoot', '申请写入目录'), ('itemId', '操作编号')]:
            if params.get(key):
                details.append(f'{label}：{params[key]}')
        for change in params.get('teamItem', {}).get('changes', []):
            details.append(f'修改文件：{change.get("path", "未知路径")}\n{change.get("diff", "（没有可用差异）")}')
        if 'fileChange' in method and not params.get('teamItem', {}).get('changes'):
            details.append('后台未提供具体文件差异；请核实申请范围后再批准。')
        details.append(f'输入 /allow {number} 批准一次，或 /deny {number} 拒绝。')
        self.emit(name, 'approval', '\n'.join(details))
        try:
            return await future
        finally:
            self.pending.pop(number, None)

    def answer(self, text):
        try:
            command, number = text.split()
            future = self.pending.get(int(number))
        except ValueError:
            return False
        if not future or future.done():
            return False
        future.set_result('accept' if command == '/allow' else 'decline')
        return True

    def cancel(self):
        for future in list(self.pending.values()):
            if not future.done():
                future.set_result('decline')


def plain_emit(source, kind, text, show_commands=False):
    if kind == 'command' and not show_commands:
        return
    print(f'[{LABELS.get(source, source)} · {kind}] {safe_text(text)}', flush=True)


async def run(args, configs, initial, interactive):
    journal = Journal(args.session_dir)
    terminal = Terminal(show_commands=args.verbose) if interactive else None
    display = terminal.emit if terminal else (
        lambda source, kind, text: plain_emit(source, kind, text, args.verbose)
    )

    def emit(source, kind, text):
        # Credentials are only inherited by the backend process, never recorded as configuration.
        for config in configs:
            for key, value in config.env.items():
                if any(word in key.upper() for word in ('KEY', 'TOKEN', 'SECRET')) and len(value) >= 8:
                    text = str(text).replace(value, '[已隐藏凭据]')
        journal.record(source, kind, text)
        display(source, kind, text)

    approvals = Approvals(emit, interactive)
    def member_emit(source, kind, text):
        team.member_event(source, kind, text)

    members = {config.name: CodexBackend(config.name, config.command, config.env, args.directory, member_emit,
                                       approval=approvals.request, timeout=args.timeout, overrides=config.overrides) for config in configs}
    team = Team(members, emit, read_only=args.read_only, cwd=args.directory)
    try:
        if args.check_connection:
            emit('team', 'stage', '正在分别连接 GPT 和 DeepSeek；只读检查，不操作项目文件。')
            async def check(name, member):
                try:
                    await member.start()
                    response = await member.turn('只回复“连接正常”。不要调用工具。')
                    visible, _ = parse_handoff(response)
                    emit(name, 'done', visible or '连接正常')
                    return True
                except Exception as error:
                    emit(name, 'error', str(error))
                    return False
            results = await asyncio.gather(*(check(name, member) for name, member in members.items()))
            return 0 if all(results) else 1
        if not interactive:
            await team.submit(initial)
            result = await team.wait()
            return 0 if result and result['ok'] else 1
        with terminal:
            loop = asyncio.get_running_loop()
            loop.add_signal_handler(signal.SIGINT, terminal.queue.put_nowait, '/stop')
            loop.add_signal_handler(signal.SIGTERM, terminal.queue.put_nowait, '/quit')
            emit('team', 'welcome', f'两位成员已配置 · 自然接力 · 无固定轮数\n项目：{args.directory}\n直接输入消息；/verbose 显示工具命令，/help 查看指令。')
            emit('team', 'status', f'公开会话记录：{journal.path}')
            try:
                while True:
                    text = await terminal.read()
                    if text is None or text.strip() in ('/quit', '/exit'):
                        break
                    text = text.strip()
                    if not text:
                        continue
                    if text == '/help':
                        emit('team', 'help', HELP)
                    elif text == '/status':
                        emit('team', 'status', team.status())
                    elif text == '/verbose':
                        enabled = terminal.toggle_commands()
                        emit('team', 'status', '工具命令显示：' + ('开启' if enabled else '关闭'))
                    elif text == '/stop':
                        approvals.cancel()
                        await team.stop()
                        emit('team', 'status', '已停止，输入新任务即可继续。')
                    elif text.startswith(('/allow', '/deny')):
                        if not approvals.answer(text):
                            emit('team', 'status', '没有这项待审批操作；使用 /allow N 或 /deny N。')
                    elif text.startswith('/'):
                        emit('team', 'status', '未知指令；/help 查看可用指令。')
                    else:
                        await team.submit(text)
            finally:
                approvals.cancel()
                await team.close()
                loop.remove_signal_handler(signal.SIGINT)
                loop.remove_signal_handler(signal.SIGTERM)
    finally:
        approvals.cancel()
        await team.close()
        journal.close()
    return 0


def main():
    parser = argparse.ArgumentParser(
        description='共享终端小组：GPT 与 DeepSeek 自然接力，命令默认隐藏。',
        epilog=HELP,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('task', nargs='?', help='执行一项任务后退出；省略则进入交互终端')
    parser.add_argument('-C', '--directory', type=Path, default=Path.cwd(), help='项目目录')
    parser.add_argument('--doctor', action='store_true', help='仅检查本机配置，不调用模型')
    parser.add_argument('--check-connection', action='store_true', help='实际请求两个模型，验证连接（会产生模型用量）')
    parser.add_argument('--read-only', action='store_true', help='全程保持只读')
    parser.add_argument('--verbose', action='store_true', help='显示工具命令（默认隐藏）')
    parser.add_argument('--timeout', type=float, default=900, help='每个模型回合超时秒数（默认 900）')
    parser.add_argument('--session-dir', type=Path, default=Path.home() / '.local/share/codex-team/sessions', help='公开会话记录目录')
    args = parser.parse_args()
    args.directory = args.directory.resolve()
    if not args.directory.is_dir():
        parser.error('项目目录不存在')
    if args.timeout <= 0:
        parser.error('超时必须为正数')
    interactive = args.task is None and sys.stdin.isatty() and sys.stdout.isatty() and not args.check_connection
    initial = args.task
    if not interactive and not args.doctor and not args.check_connection and initial is None:
        initial = sys.stdin.read().strip()
        if not initial:
            parser.print_help()
            return 0
    try:
        binary = shutil.which('codex')
        if not binary:
            raise ValueError('找不到官方 codex 可执行文件，请检查 PATH。')
        configs = load_settings(binary=binary)
        if args.doctor:
            print('Codex：' + binary)
            for config in configs:
                print(f'{LABELS[config.name]}：已配置（{config.model}）')
            config_home = Path(os.environ.get('CODEX_HOME', Path.home() / '.codex'))
            if os.environ.get('OPENAI_API_KEY'):
                print('GPT 登录：检测到 API Key 环境变量，尚未验证有效性。')
            elif (config_home / 'auth.json').is_file():
                print('GPT 登录：检测到现有认证文件，尚未验证有效性。')
            else:
                print('GPT 登录：未确认（未检测到认证文件或 OPENAI_API_KEY；系统凭据存储需实际连接核实）。')
            print('DeepSeek：检测到可读取凭据；尚未验证模型连通性。')
            print('使用 team --check-connection 发起实际连接检查。')
            return 0
        return asyncio.run(run(args, configs, initial, interactive))
    except KeyboardInterrupt:
        print('\n已中断。', file=sys.stderr)
        return 130
    except (OSError, ValueError, RuntimeError) as error:
        print('Team 无法启动：' + safe_text(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
