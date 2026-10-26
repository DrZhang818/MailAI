"""A small UTF-8 line editor that redraws input around asynchronous output."""
import asyncio
import codecs
import os
import re
import shutil
import sys
import termios
import tty
import unicodedata

from runtime import LABELS


def safe_text(value):
    text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', str(value))
    return ''.join(c for c in text if c in '\n\t' or ord(c) >= 32 and ord(c) != 127)


def width(text):
    return sum(0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in text)


class Terminal:
    def __init__(self, show_commands=False):
        self.queue = asyncio.Queue()
        self.buffer, self.cursor = '', 0
        self.history, self.history_index, self.draft = [], 0, ''
        self.escape = ''
        self.paste = False
        self.show_commands = show_commands
        self.decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
        self.saved = None
        self.active = False

    def __enter__(self):
        self.fd = sys.stdin.fileno()
        self.saved = termios.tcgetattr(self.fd)
        tty.setcbreak(self.fd)
        self.loop = asyncio.get_running_loop()
        self.loop.add_reader(self.fd, self._readable)
        self.active = True
        sys.stdout.write('\033[?2004h')
        self.redraw()
        return self

    def __exit__(self, *args):
        self.active = False
        self.loop.remove_reader(self.fd)
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)
        sys.stdout.write('\r\033[2K\033[?2004l\033[0m')
        sys.stdout.flush()

    def redraw(self):
        if not self.active:
            return
        available = max(8, shutil.get_terminal_size((80, 24)).columns - 7)
        start = self.cursor
        while start > 0 and width(self.buffer[start - 1:self.cursor].replace('\n', '↵')) < available - 1:
            start -= 1
        end = self.cursor
        while end < len(self.buffer) and width(self.buffer[start:end + 1].replace('\n', '↵')) < available:
            end += 1
        view = self.buffer[start:end].replace('\n', '↵')
        tail_width = width(self.buffer[self.cursor:end].replace('\n', '↵'))
        sys.stdout.write('\r\033[2K\033[1;36m你 ›\033[0m ' + view)
        if tail_width:
            sys.stdout.write(f'\033[{tail_width}D')
        sys.stdout.flush()

    def emit(self, source, kind, text):
        if kind == 'command' and not self.show_commands:
            return
        text = safe_text(text)
        label = LABELS.get(source, source)
        colors = {'gpt': '35', 'deepseek': '34', 'team': '36', 'user': '32'}
        color = '31' if kind == 'error' else colors.get(source, '37')
        tag = {'stage': '阶段', 'assignment': '分工', 'command': '命令', 'files': '修改', 'error': '未完成', 'done': '完成', 'status': '状态'}.get(kind, '发言')
        if self.active:
            sys.stdout.write('\r\033[2K')
        sys.stdout.write(f'\033[1;{color}m{label} · {tag}\033[0m\n')
        for line in text.splitlines() or ['']:
            sys.stdout.write('  ' + line + '\n')
        sys.stdout.write('\n')
        self.redraw()
        sys.stdout.flush()

    def toggle_commands(self):
        self.show_commands = not self.show_commands
        return self.show_commands

    async def read(self):
        return await self.queue.get()

    def _readable(self):
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            data = b''
        if not data:
            self.loop.remove_reader(self.fd)
            self.queue.put_nowait(None)
            return
        for character in self.decoder.decode(data):
            self._character(character)
        self.redraw()

    def _character(self, character):
        if self.escape or character == '\x1b':
            self.escape += character
            sequences = {'\x1b[A': 'up', '\x1b[B': 'down', '\x1b[C': 'right', '\x1b[D': 'left',
                         '\x1b[H': 'home', '\x1b[F': 'end', '\x1b[1~': 'home', '\x1b[4~': 'end',
                         '\x1b[3~': 'delete', '\x1b[200~': 'paste_start', '\x1b[201~': 'paste_end'}
            action = sequences.get(self.escape)
            if action:
                self.escape = ''
                self._action(action)
            elif not any(sequence.startswith(self.escape) for sequence in sequences):
                self.escape = ''
            return
        if self.paste:
            if character in '\r\n':
                character = '\n'
            elif ord(character) < 32 and character != '\t':
                return
            self._insert(character)
        elif character in '\r\n':
            text = self.buffer.strip()
            self.buffer, self.cursor = '', 0
            if text:
                self.history.append(text)
                self.history_index = len(self.history)
                self.queue.put_nowait(text)
        elif character in ('\x7f', '\b') and self.cursor:
            self.buffer = self.buffer[:self.cursor - 1] + self.buffer[self.cursor:]
            self.cursor -= 1
        elif character == '\x04' and not self.buffer:
            self.queue.put_nowait(None)
        elif character == '\x01':
            self.cursor = 0
        elif character == '\x05':
            self.cursor = len(self.buffer)
        elif character == '\x15':
            self.buffer, self.cursor = self.buffer[self.cursor:], 0
        elif character == '\x17':
            left = self.buffer[:self.cursor].rstrip()
            cut = max(left.rfind(' '), left.rfind('\n')) + 1
            self.buffer = self.buffer[:cut] + self.buffer[self.cursor:]
            self.cursor = cut
        elif ord(character) >= 32:
            self._insert(character)

    def _insert(self, text):
        self.buffer = self.buffer[:self.cursor] + text + self.buffer[self.cursor:]
        self.cursor += len(text)

    def _action(self, action):
        if action in ('paste_start', 'paste_end'):
            self.paste = action == 'paste_start'
        elif action == 'left':
            self.cursor = max(0, self.cursor - 1)
        elif action == 'right':
            self.cursor = min(len(self.buffer), self.cursor + 1)
        elif action == 'home':
            self.cursor = 0
        elif action == 'end':
            self.cursor = len(self.buffer)
        elif action == 'delete':
            self.buffer = self.buffer[:self.cursor] + self.buffer[self.cursor + 1:]
        elif action in ('up', 'down') and self.history:
            if self.history_index == len(self.history):
                self.draft = self.buffer
            self.history_index = min(len(self.history), max(0, self.history_index + (-1 if action == 'up' else 1)))
            self.buffer = self.history[self.history_index] if self.history_index < len(self.history) else self.draft
            self.cursor = len(self.buffer)
