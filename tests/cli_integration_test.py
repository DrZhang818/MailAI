"""通过实际命令入口、临时 SQLite 与虚构 TLS 邮箱验证 CLI。"""

import fcntl
import contextlib
import os
import pathlib
import pty
import select
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import termios
import time

from fixtures.imap_server import ImapFixture, SAMPLE_MAIL, TEST_PASSWORD, TEST_USER


def invoke(executable, env, arguments, code):
    result = subprocess.run(
        [executable, *arguments], env=env, timeout=25,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert TEST_PASSWORD.encode() not in result.stdout + result.stderr
    assert result.returncode == code, (arguments, result.returncode, result.stderr)
    return result.stdout.decode("utf-8"), result.stderr.decode("utf-8")


def configure(env, fixture):
    return dict(env, MAILAI_EMAIL="student@example.test",
                MAILAI_IMAP_HOST="127.0.0.1", MAILAI_IMAP_USER=TEST_USER,
                MAILAI_IMAP_PORT=str(fixture.port), MAILAI_IMAP_TLS="implicit",
                MAILAI_IMAP_CA_FILE=str(fixture.ca_file),
                MAILAI_PASSWORD_ENV="MAILAI_FIXTURE_PASSWORD",
                MAILAI_FIXTURE_PASSWORD=TEST_PASSWORD)


def verify_offline(executable, env, database):
    output, error = invoke(executable, env, ["inbox"], 0)
    assert "数据来源：缓存" in output and "IMAP fixture" in output and "1 封" in output
    assert not error
    with sqlite3.connect(database) as db:
        mail_id, raw = db.execute("SELECT id, raw FROM messages").fetchone()
        assert raw == SAMPLE_MAIL
    output, error = invoke(executable, env, ["show", str(mail_id)], 0)
    assert "这是一封测试邮件。" in output and not error
    invoke(executable, env, ["show", "18446744073709551615"], 1)
    invoke(executable, env, ["show", "18446744073709551616"], 2)
    return mail_id


def verify_search_and_rich_detail(executable, env, database):
    # 模拟旧缓存中仅保存了扁平正文，但原始邮件仍包含 HTML 表格和内嵌图片。
    rich = ("From: fixture@example.test\r\nSubject: Rich example\r\n"
            "MIME-Version: 1.0\r\nContent-Type: multipart/related; boundary=rich\r\n\r\n"
            "--rich\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
            "<p>正文开头</p><table><tr><th>类别</th><th>数量</th></tr>"
            "<tr><td><p>奖学金</p><p>校级</p></td><td><p>3</p></td></tr></table>"
            "<p>正文结尾</p><img src='cid:picture'>\r\n"
            "--rich\r\nContent-Type: image/png\r\nContent-ID: <picture>\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\nAAECAwQ=\r\n--rich--\r\n").encode()
    with sqlite3.connect(database) as db:
        mail_id, = db.execute("SELECT id FROM messages").fetchone()
        db.execute("UPDATE messages SET raw=?, body=?, subject=? WHERE id=?",
                   (rich, "旧正文 %_ 搜索词", "Rich example", mail_id))
    for keyword in ["搜索词", "RICH", "%_"]:
        output, error = invoke(executable, env, ["find", keyword], 0)
        assert "Rich example" in output and "1 封" in output and not error
    output, error = invoke(executable, env, ["find", "不存在"], 0)
    assert "0 封" in output and not error
    output, error = invoke(executable, env, ["show", str(mail_id)], 0)
    assert "正文开头" in output and "正文结尾" in output and not error
    assert any("类别" in line and "数量" in line for line in output.splitlines())
    assert any("奖学金" in line and "3" in line for line in output.splitlines())
    assert "校级" in output and "\\x0A" not in output
    assert "AAECAwQ=" not in output and "旧正文" not in output
    with sqlite3.connect(database) as db:
        assert db.execute("SELECT raw,body FROM messages WHERE id=?", (mail_id,)).fetchone() == (
            rich, "旧正文 %_ 搜索词")


def test_cli(executable, directory):
    data = directory / "cache"
    database = data / "cache.sqlite3"
    env = {
        "PATH": os.environ.get("PATH", ""),
        "HOME": str(directory),
        "MAILAI_DATA_DIR": str(data),
        "NO_PROXY": "127.0.0.1",
        "no_proxy": "127.0.0.1",
    }

    # 帮助、错误参数、空缓存和示例模式都不创建目录或连接邮件源。
    for args, code in [(["--help"], 0), (["sync", "--help"], 0),
                       (["--demo", "sync"], 2), (["show", "0"], 2),
                       (["sync", "extra"], 2), (["sync"], 1)]:
        invoke(executable, env, args, code)
    output, error = invoke(executable, env, ["inbox"], 0)
    assert "数据来源：缓存" in output and "0 封" in output and not error
    invoke(executable, env, ["show", "43"], 1)
    output, error = invoke(executable, env, ["--demo", "inbox"], 0)
    assert "数据来源：示例" in output and "课程通知" in output and not error
    assert not data.exists()

    with ImapFixture(raw=None) as fixture:
        sync_env = configure(env, fixture)
        invalid = dict(sync_env, MAILAI_PASSWORD_ENV="FIXTURE_REF=fixture-only-value")
        _, error = invoke(executable, invalid, ["sync"], 1)
        assert "MAILAI_PASSWORD_ENV" in error and "fixture-only-value" not in error
        assert not data.exists() and not fixture.server.connection_times
        for args, code in [(["--help"], 0), (["sync", "extra"], 2),
                           (["inbox"], 0), (["show", "43"], 1)]:
            invoke(executable, sync_env, args, code)
        assert not data.exists() and not fixture.server.connection_times
        output, error = invoke(executable, sync_env, ["sync"], 0)
        assert "新增" in output and "成功" in output and not error
        assert database.is_file() and fixture.server.search_requests == 1
        assert stat.S_IMODE(data.stat().st_mode) == 0o700
        assert stat.S_IMODE(database.stat().st_mode) == 0o600
        assert fixture.server.fetch_requests == 0
        # 模拟之前留下的待办；服务器已移除该邮件，随后又收到新邮件。
        with sqlite3.connect(database) as db:
            db.execute("UPDATE sync_state SET last_discovered_uid=100")
            db.execute("INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
                       "VALUES ('school','INBOX',77,100)")
        fixture.server.raw = SAMPLE_MAIL
        output, error = invoke(executable, sync_env, ["sync"], 3)
        assert "新增" in output and "成功" in output and "失败" in output
        assert "100" in error and "remote_gone" in error
        assert fixture.server.fetch_requests == 2 and not fixture.server.seen
        mail_id = verify_offline(executable, env, database)
        before = (len(fixture.server.connection_times), fixture.server.search_requests,
                  fixture.server.fetch_requests)
        assert verify_offline(executable, sync_env, database) == mail_id
        assert before == (len(fixture.server.connection_times), fixture.server.search_requests,
                          fixture.server.fetch_requests)
        invoke(executable, sync_env, ["sync"], 0)
        assert fixture.server.fetch_requests == 2
        assert verify_offline(executable, env, database) == mail_id
        with sqlite3.connect(database) as db:
            assert db.execute("SELECT count(*) FROM messages").fetchone() == (1,)
            assert db.execute("SELECT retryable FROM sync_pending WHERE uid=100").fetchone() == (0,)
        # 写锁被占用时，查询仍能执行，同步必须在连接服务器前报忙。
        with (data / ".mailai.lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            verify_offline(executable, env, database)
            before = len(fixture.server.connection_times)
            _, error = invoke(executable, sync_env, ["sync"], 1)
            assert "busy" in error and len(fixture.server.connection_times) == before
        invoke(executable, sync_env, ["sync"], 0)
        assert not fixture.server.seen
    verify_offline(executable, env, database)
    verify_search_and_rich_detail(executable, env, database)

    # 缓存损坏不能被解释为空邮箱或替换为示例。
    bad = directory / "corrupt"
    bad.mkdir()
    (bad / "cache.sqlite3").write_bytes(b"not a SQLite database")
    out, err = invoke(executable, dict(env, MAILAI_DATA_DIR=str(bad)), ["inbox"], 1)
    assert not out and err


def test_history_cli(executable, directory):
    for name, args in [("limited", ["sync", "--history", "1"]),
                       ("all", ["sync", "--all"])]:
        data = directory / name
        database = data / "cache.sqlite3"
        env = {
            "PATH": os.environ.get("PATH", ""),
            "HOME": str(directory),
            "MAILAI_DATA_DIR": str(data),
            "NO_PROXY": "127.0.0.1",
            "no_proxy": "127.0.0.1",
        }
        with ImapFixture(raw=None) as fixture:
            sync_env = configure(env, fixture)
            invoke(executable, sync_env, ["sync"], 0)
            # 模拟先前的发现水位已越过一封未缓存的历史邮件。
            with sqlite3.connect(database) as db:
                db.execute("UPDATE sync_state SET last_discovered_uid=200")
            fixture.server.raw = SAMPLE_MAIL
            output, error = invoke(executable, sync_env, ["sync"], 0)
            assert "新增 0 封" in output and not error
            assert fixture.server.fetch_requests == 0
            output, error = invoke(executable, sync_env, args, 0)
            assert "新增 1 封，成功 1 封，失败 0 封" in output and not error
            assert fixture.server.fetch_uids == ["101"] and not fixture.server.seen
            first_id = verify_offline(executable, env, database)
            for repeated in (["sync"], args, ["sync", "--all"]):
                output, error = invoke(executable, sync_env, repeated, 0)
                assert "新增 0 封" in output and not error
            assert fixture.server.fetch_uids == ["101"]
            assert verify_offline(executable, env, database) == first_id
            with sqlite3.connect(database) as db:
                assert db.execute("SELECT last_discovered_uid FROM sync_state").fetchone() == (200,)


def test_cache_permissions(executable, directory):
    data = directory / "permission-cache" / "mail"
    database = data / "cache.sqlite3"
    env = {
        "PATH": os.environ.get("PATH", ""),
        "HOME": str(directory),
        # 目录末尾带分隔符也必须按应用目录创建，不能使用父目录默认权限。
        "MAILAI_DATA_DIR": str(data) + "/",
        "NO_PROXY": "127.0.0.1",
        "no_proxy": "127.0.0.1",
    }
    with ImapFixture() as fixture:
        sync_env = configure(env, fixture)
        # 新建目录和数据库的私有权限不依赖调用者的默认 umask。
        old_mask = os.umask(0o022)
        try:
            invoke(executable, sync_env, ["sync"], 0)
        finally:
            os.umask(old_mask)
        assert stat.S_IMODE(data.stat().st_mode) == 0o700, "new cache directory must be private"
        assert stat.S_IMODE(database.stat().st_mode) == 0o600, "new cache file must be private"
        mail_id = verify_offline(executable, env, database)

        # 兼容原来的缓存，只修复应用自己的文件，不改变已有目录及其他文件权限。
        data.chmod(0o755)
        database.chmod(0o644)
        unrelated = data / "unrelated.txt"
        unrelated.write_text("synthetic unrelated file")
        unrelated.chmod(0o644)
        with contextlib.closing(sqlite3.connect(database)) as reader:
            reader.execute("BEGIN")
            assert reader.execute("SELECT id FROM messages").fetchone() == (mail_id,)
            sidecars = [data / "cache.sqlite3-wal", data / "cache.sqlite3-shm"]
            for path in sidecars:
                path.chmod(0o644)
            invoke(executable, sync_env, ["sync"], 0)
            assert stat.S_IMODE(database.stat().st_mode) == 0o600
            assert all(stat.S_IMODE(path.stat().st_mode) == 0o600 for path in sidecars)
        assert stat.S_IMODE(data.stat().st_mode) == 0o755
        assert stat.S_IMODE(unrelated.stat().st_mode) == 0o644
        assert verify_offline(executable, env, database) == mail_id
        assert fixture.server.fetch_requests == 1

        # 其他用户可写的目录不是安全的缓存位置；不得在该目录继续同步。
        data.chmod(0o777)
        try:
            before = len(fixture.server.connection_times)
            invoke(executable, sync_env, ["sync"], 1)
            assert len(fixture.server.connection_times) == before
            assert stat.S_IMODE(data.stat().st_mode) == 0o777
        finally:
            data.chmod(0o700)


def test_login_terminal(executable, directory):
    # 只用错误的虚构密码验证真实终端交互，不会保存任何钥匙串凭据。
    env = {
        "PATH": os.environ.get("PATH", ""),
        "HOME": str(directory),
        "MAILAI_DATA_DIR": str(directory / "login-cache"),
        "NO_PROXY": "127.0.0.1",
        "no_proxy": "127.0.0.1",
    }
    secret = b"synthetic-invalid-login-password"
    with ImapFixture() as fixture:
        env = configure(env, fixture)
        pid, fd = pty.fork()
        if pid == 0:
            os.execve(executable, [executable, "login"], env)
        received = bytearray()
        cursor = 0
        reaped = False
        deadline = time.monotonic() + 15

        def read_more():
            remaining = deadline - time.monotonic()
            assert remaining > 0, "interactive login timed out"
            ready, _, _ = select.select([fd], [], [], remaining)
            assert ready, "interactive login timed out"
            try:
                data = os.read(fd, 4096)
            except OSError:
                return False
            received.extend(data)
            return bool(data)

        def expect_prompt(label):
            nonlocal cursor
            marker = label.encode()
            while (index := received.find(marker, cursor)) < 0:
                assert read_more(), (label, "login stopped before prompt")
            cursor = index + len(marker)

        try:
            for label in ("邮箱地址", "IMAP 服务器", "IMAP 用户名",
                          "TLS 模式", "IMAP 端口", "邮件数据目录"):
                expect_prompt(label)
                os.write(fd, b"\n")
            expect_prompt("邮箱密码或授权码")
            # 等待读取密码的终端关闭回显后再模拟用户输入。
            while termios.tcgetattr(fd)[3] & termios.ECHO:
                assert time.monotonic() < deadline, "password echo was not disabled"
                time.sleep(0.01)
            os.write(fd, secret + b"\n")
            while read_more():
                pass
            _, status = os.waitpid(pid, 0)
            reaped = True
            assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 1
            assert secret not in received and TEST_PASSWORD.encode() not in received
            assert b"auth_error" in received
            assert not (directory / ".config" / "mail-ai-assistant" / "login.conf").exists()
            assert not (directory / "login-cache").exists()
            assert fixture.server.search_requests == 0 and not fixture.server.seen
        finally:
            os.close(fd)
            if not reaped:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, 0)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="mailai-cli-") as temporary:
        test_cli(sys.argv[1], pathlib.Path(temporary))
        test_history_cli(sys.argv[1], pathlib.Path(temporary))
        test_cache_permissions(sys.argv[1], pathlib.Path(temporary))
        if sys.platform == "darwin":
            login_home = pathlib.Path(temporary) / "login-home"
            login_home.mkdir()
            test_login_terminal(sys.argv[1], login_home)
