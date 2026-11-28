"""通过 C++ 的 UID FETCH 实现下载虚构邮件。"""

import sys

from fixtures.imap_server import ImapFixture, SAMPLE_MAIL, TEST_PASSWORD
from imap_connection_test import run_client


def fetch(executable, fixture, mode, scenario, password=TEST_PASSWORD, expected_connections=None):
    run_client(executable, fixture, mode, "fetch-" + scenario, password, expected_connections)
    assert not fixture.server.seen, "UID FETCH should preserve unread status"
    assert all("BODY.PEEK[]" in items and "INTERNALDATE" in items
               for items in fixture.server.fetch_items)


if __name__ == "__main__":
    executable = sys.argv[1]
    for mode in ("implicit", "starttls"):
        with ImapFixture(implicit_tls=mode == "implicit") as fixture:
            fetch(executable, fixture, mode, "ok")
            assert fixture.server.fetch_requests == 1
            fetch(executable, fixture, mode, "missing-uid")
            assert fixture.server.fetch_requests == 2
            run_client(executable, fixture, mode, "session-reuse", TEST_PASSWORD, expected_connections=1)
            assert fixture.server.fetch_requests == 5 and not fixture.server.seen
            for scenario in ("invalid-account", "invalid-folder", "invalid-uid", "invalid-validity"):
                fetch(executable, fixture, mode, scenario, expected_connections=0)
            fetch(executable, fixture, mode, "wrong-password", "wrong-test-password", 1)
            fetch(executable, fixture, mode, "missing-password", None, 0)
            fetch(executable, fixture, mode, "untrusted", expected_connections=1)
            assert fixture.server.fetch_requests == 5
        with ImapFixture(implicit_tls=mode == "implicit", certificate_san="DNS:localhost") as fixture:
            fetch(executable, fixture, mode, "wrong-hostname", expected_connections=1)
            assert fixture.server.fetch_requests == 0

    with ImapFixture(implicit_tls=False, allow_starttls=False) as fixture:
        fetch(executable, fixture, "starttls", "no-starttls", expected_connections=1)
        assert fixture.server.fetch_requests == 0

    for scenario, options in (
        ("stale", {"uid_validity": 78}),
        ("empty-mailbox", {"raw": None}),
        ("missing-validity", {"uid_validity": None}),
        ("body-first", {"fetch_mode": "body-first"}),
        ("flags-before", {"fetch_mode": "flags-before"}),
        ("binary", {"raw": SAMPLE_MAIL + b"\x00\xff\x01\r\n"}),
        ("missing-closure", {"raw": b"", "fetch_mode": "missing-closure"}),
        ("no-final-crlf", {"raw": SAMPLE_MAIL[:-2]}),
        ("wrong-uid", {"fetch_mode": "wrong-uid"}),
        ("missing-date", {"fetch_mode": "missing-date"}),
        ("invalid-date", {"fetch_mode": "invalid-date"}),
        ("missing-body", {"fetch_mode": "missing-body"}),
        ("rejected", {"fetch_mode": "rejected"}),
        ("timeout", {"fetch_mode": "timeout"}),
        ("at-limit", {"raw": SAMPLE_MAIL.ljust(20 * 1024 * 1024, b"a")}),
        ("over-limit", {"raw": SAMPLE_MAIL.ljust(20 * 1024 * 1024 + 1, b"a")}),
    ):
        with ImapFixture(**options) as fixture:
            attempts = 3 if scenario == "timeout" else 1
            fetch(executable, fixture, "implicit", scenario, expected_connections=attempts)
            expected_requests = 0 if scenario in ("stale", "missing-validity") else attempts
            assert fixture.server.fetch_requests == expected_requests, scenario
