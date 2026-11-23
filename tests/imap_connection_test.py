"""通过虚构的 TLS 邮箱验证实际 libcurl 句柄。"""

import os
import subprocess
import sys

from fixtures.imap_server import ImapFixture, TEST_PASSWORD


def run_client(executable, server, mode, scenario, password, expected_connections=None):
    env = dict(os.environ)
    env.update(NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    env.pop("MAILAI_FIXTURE_PASSWORD", None)
    if password is not None:
        env["MAILAI_FIXTURE_PASSWORD"] = password
    connections_before = len(server.server.connection_times)
    result = subprocess.run(
        [executable, str(server.port), str(server.ca_file), mode, scenario],
        env=env, timeout=25, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    for secret in (password, TEST_PASSWORD):
        if secret:
            assert secret.encode() not in result.stdout and secret.encode() not in result.stderr
    assert result.returncode == 0, (scenario, result.stderr.decode(errors="replace"))
    if scenario.startswith(("snapshot-", "fetch-")):
        assert result.stdout == b"", "IMAP query should not write to stdout"
    if expected_connections is not None:
        assert len(server.server.connection_times) - connections_before == expected_connections, scenario


if __name__ == "__main__":
    executable = sys.argv[1]
    for mode in ("implicit", "starttls"):
        with ImapFixture(implicit_tls=mode == "implicit") as fixture:
            run_client(executable, fixture, mode, "ok", TEST_PASSWORD)
            run_client(executable, fixture, mode, "wrong-password", "wrong-test-password")
            run_client(executable, fixture, mode, "missing-password", None)
            run_client(executable, fixture, mode, "untrusted", TEST_PASSWORD)
            run_client(executable, fixture, mode, "snapshot-ok", TEST_PASSWORD)
            assert not fixture.server.seen and fixture.server.search_requests == 1
            run_client(executable, fixture, mode, "snapshot-wrong-password", "wrong-test-password", 1)
            run_client(executable, fixture, mode, "snapshot-missing-password", None, 0)
            run_client(executable, fixture, mode, "snapshot-untrusted", TEST_PASSWORD, 1)
        with ImapFixture(implicit_tls=mode == "implicit", certificate_san="DNS:localhost") as fixture:
            run_client(executable, fixture, mode, "wrong-hostname", TEST_PASSWORD)
            run_client(executable, fixture, mode, "snapshot-wrong-hostname", TEST_PASSWORD, 1)
        with ImapFixture(implicit_tls=mode == "implicit", raw=None) as fixture:
            run_client(executable, fixture, mode, "snapshot-empty", TEST_PASSWORD)
            assert fixture.server.search_requests == 1

    for scenario in ("rejected", "malformed", "timeout"):
        with ImapFixture(search_mode=scenario) as fixture:
            attempts = 3 if scenario == "timeout" else 1
            run_client(executable, fixture, "implicit", "snapshot-" + scenario, TEST_PASSWORD, attempts)
            assert fixture.server.search_requests == attempts

    with ImapFixture(implicit_tls=False, allow_starttls=False) as fixture:
        run_client(executable, fixture, "starttls", "snapshot-no-starttls", TEST_PASSWORD, 1)
        assert fixture.server.search_requests == 0
