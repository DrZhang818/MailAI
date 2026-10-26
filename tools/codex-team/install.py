#!/usr/bin/env python3
"""Install a self-contained entry without changing shell or model configuration."""
import argparse
import os
from pathlib import Path
import shlex
import sys
import tempfile

MARKER = '# codex-team managed launcher v1'
FILES = ('backend.py', 'settings.py', 'runtime.py', 'terminal.py', 'cli.py', 'install.py', 'README.md')


def atomic_write(path, content, mode):
    fd, temporary = tempfile.mkstemp(prefix='.team-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            stream.write(content)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description='安装/卸载统一终端 team，不改动 codex 和已有配置。')
    parser.add_argument('--home', type=Path, default=Path.home(), help='安装用户目录')
    parser.add_argument('--uninstall', action='store_true')
    args = parser.parse_args()
    home = args.home.resolve()
    launcher = home / '.local/bin/team'
    data = home / '.local/share/codex-team'
    try:
        if launcher.is_symlink() or launcher.exists() and MARKER not in launcher.read_text():
            raise ValueError(f'已有其他 team 命令，未覆盖：{launcher}')
        if args.uninstall:
            if launcher.is_file():
                launcher.unlink()
            print('已移除 team 入口；程序与会话记录保留在 ' + str(data))
            return 0
        if data.is_symlink():
            raise ValueError('安装目录是符号链接，停止安装。')
        source = Path(__file__).resolve().parent
        contents = {name: (source / name).read_text() for name in FILES}
        launcher.parent.mkdir(parents=True, exist_ok=True)
        data.mkdir(parents=True, exist_ok=True, mode=0o700)
        for name, content in contents.items():
            atomic_write(data / name, content, 0o600)
        script = f'#!/bin/sh\n{MARKER}\nexec {shlex.quote(sys.executable)} {shlex.quote(str(data / "cli.py"))} "$@"\n'
        atomic_write(launcher, script, 0o755)
        print('已安装：' + str(launcher))
        return 0
    except (OSError, ValueError) as error:
        print('Team 安装失败：' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
