"""Reuse the user's existing Codex configuration without rewriting it."""
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import shutil
import tomllib


@dataclass
class BackendConfig:
    name: str
    command: list[str]
    env: dict[str, str]
    model: str
    overrides: dict = field(default_factory=dict)


def load_settings(home=None, environ=None, binary=None):
    home = Path(home) if home is not None else Path.home()
    environ = dict(os.environ if environ is None else environ)
    binary = binary or shutil.which('codex') or 'codex'
    config_home = Path(environ.get('CODEX_HOME', home / '.codex'))
    profile = config_home / 'deepseek-menu.config.toml'
    if not profile.is_file():
        raise ValueError(f'找不到现有 DeepSeek 配置：{profile}')
    key = environ.get('DEEPSEEK_API_KEY', '').strip()
    credentials = home / '.local/share/codex-menu/credentials.json'
    if not key and credentials.is_file():
        try:
            key = json.loads(credentials.read_text()).get('api_key', '').strip()
        except (ValueError, AttributeError, OSError) as error:
            raise ValueError('现有 DeepSeek 凭据文件无法读取，请通过 DEEPSEEK_API_KEY 设置密钥。') from error
    if not key:
        raise ValueError('未找到 DeepSeek 密钥；先使用现有 codex 菜单配置，或设置 DEEPSEEK_API_KEY。')
    gpt_path = config_home / 'config.toml'
    try:
        gpt = tomllib.loads(gpt_path.read_text()) if gpt_path.is_file() else {}
        deepseek = tomllib.loads(profile.read_text())
    except (OSError, ValueError) as error:
        raise ValueError('现有 Codex TOML 配置无法读取。') from error
    # No nested delegation: the two members negotiate ownership in this runtime.
    common = ['-c', 'agents.enabled=false', '-c', 'skills.include_instructions=false',
              'app-server', '--listen', 'stdio://']
    gpt_env = dict(environ)
    gpt_env.pop('DEEPSEEK_API_KEY', None)
    deepseek_env = dict(environ, DEEPSEEK_API_KEY=key)
    return [BackendConfig('gpt', [binary, *common], gpt_env, gpt.get('model', '当前默认模型')),
            BackendConfig('deepseek', [binary, *common],
                          deepseek_env, deepseek.get('model', '当前默认模型'), deepseek)]
