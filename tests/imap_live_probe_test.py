"""仅用本机虚构邮箱验证真实探针的启用条件、取样范围及输出脱敏。"""

import os
import pathlib
import subprocess
import sys
import tempfile

from fixtures.imap_server import ImapFixture, TEST_PASSWORD, TEST_USER


def run_probe(executable, fixture, directory, enabled, expected, password=TEST_PASSWORD):
    env = {
        "PATH": os.environ.get("PATH", ""),
        "HOME": str(directory),
        "MAILAI_DATA_DIR": str(directory / "unused-cache"),
        "MAILAI_EMAIL": "fixture@example.test",
        "MAILAI_ACCOUNT_ID": "probe-account-private",
        "MAILAI_IMAP_HOST": "127.0.0.1",
        "MAILAI_IMAP_USER": TEST_USER,
        "MAILAI_IMAP_PORT": str(fixture.port),
        "MAILAI_IMAP_CA_FILE": str(fixture.ca_file),
        "MAILAI_PASSWORD_ENV": "MAILAI_FIXTURE_PASSWORD",
        "MAILAI_FIXTURE_PASSWORD": password,
        "NO_PROXY": "127.0.0.1",
        "no_proxy": "127.0.0.1",
    }
    if enabled is not None:
        env["MAILAI_LIVE_TESTS"] = enabled
    result = subprocess.run(
        [executable], env=env, timeout=25, capture_output=True,
    )
    for private in (password, TEST_PASSWORD, TEST_USER, "probe-account-private", "fixture@example.test",
                    "sender@example.test", "IMAP fixture", "这是一封测试邮件。"):
        assert private.encode() not in result.stdout + result.stderr
    assert not (directory / "unused-cache").exists()
    assert result.returncode == expected, (result.returncode, result.stderr)
    return result.stdout.decode(), result.stderr.decode()


if __name__ == "__main__":
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="mailai-probe-") as temporary:
        directory = pathlib.Path(temporary)
        with ImapFixture() as fixture:
            for disabled in (None, "0", "true"):
                run_probe(executable, fixture, directory, disabled, 2)
                assert not fixture.server.connection_times
            out, err = run_probe(executable, fixture, directory, "1", 0)
            assert "尝试 1" in out and "解析成功 1" in out and not err
            assert fixture.server.fetch_uids == ["101"] and not fixture.server.seen
            assert "BODY.PEEK[]" in fixture.server.fetch_items[0]
        with ImapFixture(raw=None) as fixture:
            out, err = run_probe(executable, fixture, directory, "1", 0)
            assert "尝试 0" in out and "解析成功 0" in out and not err
            assert not fixture.server.fetch_requests
        with ImapFixture() as fixture:
            fixture.server.search_uids = [101, 102, 103]
            out, err = run_probe(executable, fixture, directory, "1", 3)
            assert len(fixture.server.connection_times) == 1
            assert "尝试 3" in out and "解析成功 1" in out and "失败 2" in out
            assert "remote_gone" in err and not fixture.server.seen
        with ImapFixture() as fixture:
            out, err = run_probe(executable, fixture, directory, "1", 1,
                                 password="wrong-fixture-password")
            assert not out and "auth_error" in err and not fixture.server.fetch_requests
        with ImapFixture() as fixture:
            # 查询结果中其余 UID 均已消失，仍只能尝试最大的五个不同 UID。
            fixture.server.search_uids = [107, 101, 102, 103, 104, 105, 106, 107]
            out, err = run_probe(executable, fixture, directory, "1", 1)
            assert len(fixture.server.connection_times) == 1
            assert "尝试 5" in out and "失败 5" in out and "remote_gone" in err
            assert fixture.server.fetch_uids == ["103", "104", "105", "106", "107"]
            assert not fixture.server.seen
        with ImapFixture(raw=b"Content-Type: text/plain; charset=x-mailai-unknown\r\n\r\nHello") as fixture:
            out, err = run_probe(executable, fixture, directory, "1", 1)
            assert "下载成功 1" in out and "解析成功 0" in out and "parse_error" in err
            assert not fixture.server.seen
    print("Live probe fixture tests passed")
