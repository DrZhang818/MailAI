"""验证实际 TLS 连接出现临时故障后的有限重试和远端标识校验。"""

import socket
import sys

from fixtures.imap_server import ImapFixture, TEST_PASSWORD, _Handler
from imap_connection_test import run_client
from imap_fetch_test import fetch


def check_backoff(server):
    for index, failure_time in enumerate(server.failure_times[:2]):
        if index + 1 < len(server.connection_times):
            assert server.connection_times[index + 1] - failure_time >= (1, 2)[index] - 0.05


class ExistsBeforeFetch(_Handler):
    def _uid_command(self, tag, arguments):
        if arguments.upper().startswith("FETCH "):
            self._send("* 2 EXISTS\r\n")
        return super()._uid_command(tag, arguments)


class IdleDisconnectAfterSearch(_Handler):
    def _uid_command(self, tag, arguments):
        keep_open = super()._uid_command(tag, arguments)
        if arguments.upper() == "SEARCH ALL":
            self.server.uid_validity = self.server.reconnect_validity
            self.transport.shutdown(socket.SHUT_WR)
            return False
        return keep_open

    def _send(self, message):
        # 重连后的 SELECT 省略访问模式响应代码。复用句柄时依靠新连接计数检测重连。
        if len(self.server.connection_times) > 1:
            message = message.replace(" OK [READ-WRITE] SELECT", " OK SELECT")
        super()._send(message)


if __name__ == "__main__":
    executable = sys.argv[1]
    for mode in ("implicit", "starttls"):
        implicit_tls = mode == "implicit"
        with ImapFixture(implicit_tls=implicit_tls, search_mode="disconnect", search_failures=1) as fixture:
            run_client(executable, fixture, mode, "snapshot-ok", TEST_PASSWORD, 2)
            assert fixture.server.search_requests == 2
            assert fixture.server.select_validities == [77, 77]
            check_backoff(fixture.server)
        with ImapFixture(implicit_tls=implicit_tls, fetch_mode="disconnect", fetch_failures=2) as fixture:
            fetch(executable, fixture, mode, "ok", expected_connections=3)
            assert fixture.server.fetch_requests == 3
            assert fixture.server.select_validities == [77, 77, 77]
            check_backoff(fixture.server)
        with ImapFixture(implicit_tls=implicit_tls, fetch_mode="disconnect-stale") as fixture:
            fetch(executable, fixture, mode, "stale", expected_connections=2)
            assert fixture.server.fetch_requests == 1
            assert fixture.server.select_validities == [77, 78]
        with ImapFixture(implicit_tls=implicit_tls, fetch_mode="disconnect-gone") as fixture:
            fetch(executable, fixture, mode, "empty-mailbox", expected_connections=2)
            assert fixture.server.fetch_requests == 2
            assert fixture.server.select_validities == [77, 77]
        with ImapFixture(implicit_tls=implicit_tls, fetch_mode="disconnect", fetch_failures=1) as fixture:
            run_client(executable, fixture, mode, "session-reconnect", TEST_PASSWORD, 2)
            assert fixture.server.fetch_requests == 3 and not fixture.server.seen
            check_backoff(fixture.server)
        with ImapFixture(implicit_tls=implicit_tls, fetch_mode="disconnect-stale") as fixture:
            run_client(executable, fixture, mode, "session-stale-reconnect", TEST_PASSWORD, 2)
            assert fixture.server.fetch_requests == 1
            check_backoff(fixture.server)
        with ImapFixture(implicit_tls=implicit_tls) as fixture:
            fixture.server.RequestHandlerClass = ExistsBeforeFetch
            run_client(executable, fixture, mode, "session-reuse", TEST_PASSWORD, 1)
            assert fixture.server.select_validities == [77]
            assert fixture.server.fetch_requests == 3 and not fixture.server.seen
        for validity, suffix, fetches in ((77, "", 1), (None, "-missing", 1), (78, "-stale", 0)):
            with ImapFixture(implicit_tls=implicit_tls) as fixture:
                fixture.server.RequestHandlerClass = IdleDisconnectAfterSearch
                fixture.server.reconnect_validity = validity
                scenario = "session-idle-reconnect" + suffix
                run_client(executable, fixture, mode, scenario, TEST_PASSWORD)
                assert len(fixture.server.connection_times) >= 2, scenario
                assert fixture.server.search_requests == 1
                assert fixture.server.select_validities == [77, validity]
                assert fixture.server.fetch_requests == fetches and not fixture.server.seen

    for options, operation, attempts in (
        ({"connect_failures": 1}, "snapshot-ok", 2),
        ({"search_mode": "disconnect"}, "snapshot-network-error", 3),
        ({"search_mode": "unavailable", "search_failures": 1}, "snapshot-ok", 2),
        ({"search_mode": "unavailable"}, "snapshot-network-error", 3),
        ({"search_mode": "timeout", "search_failures": 1}, "snapshot-timeout-recover", 2),
    ):
        with ImapFixture(**options) as fixture:
            run_client(executable, fixture, "implicit", operation, TEST_PASSWORD, attempts)
            assert fixture.server.search_requests == attempts - options.get("connect_failures", 0)
            check_backoff(fixture.server)

    for mode, failures, scenario, attempts in (
        ("disconnect", None, "network-error", 3),
        ("unavailable", 1, "ok", 2),
        ("unavailable-after-body", 1, "ok", 2),
        ("unavailable", None, "network-error", 3),
        ("timeout", 1, "timeout-recover", 2),
    ):
        with ImapFixture(fetch_mode=mode, fetch_failures=failures) as fixture:
            fetch(executable, fixture, "implicit", scenario, expected_connections=attempts)
            assert fixture.server.fetch_requests == attempts
            assert fixture.server.select_validities == [77] * attempts
            check_backoff(fixture.server)
