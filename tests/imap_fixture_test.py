"""在测试 C++ 客户端之前验证本地 IMAP 测试服务器。"""

import imaplib
import ssl

from fixtures.imap_server import ImapFixture, SAMPLE_MAIL, TEST_PASSWORD, TEST_USER


def check_mailbox(implicit_tls):
    with ImapFixture(implicit_tls=implicit_tls) as server:
        context = ssl.create_default_context(cafile=str(server.ca_file))
        if implicit_tls:
            client = imaplib.IMAP4_SSL("127.0.0.1", server.port, ssl_context=context, timeout=5)
        else:
            client = imaplib.IMAP4("127.0.0.1", server.port, timeout=5)
            client.starttls(context)

        with client:
            try:
                client.login(TEST_USER, "wrong-test-password")
            except imaplib.IMAP4.error:
                pass
            else:
                raise AssertionError("an incorrect password was accepted")
            assert client.login(TEST_USER, TEST_PASSWORD)[0] == "OK"
            assert client.select("INBOX")[1] == [b"1"]
            assert client.response("UIDVALIDITY")[1] == [b"77"]
            assert client.uid("SEARCH", None, "ALL")[1] == [b"101"]

            status, before = client.uid("FETCH", "101", "(FLAGS)")
            assert status == "OK" and b"\\Seen" not in before[0]
            status, fetched = client.uid("FETCH", "101", "(INTERNALDATE BODY.PEEK[])")
            assert status == "OK" and fetched[0][1] == SAMPLE_MAIL
            assert b'INTERNALDATE "21-Nov-2026 10:00:00 +0800"' in fetched[0][0]
            status, after = client.uid("FETCH", "101", "(FLAGS)")
            assert status == "OK" and b"\\Seen" not in after[0]

            # 对照测试：普通 BODY[] 下载会将样本标记为已读。
            assert client.uid("FETCH", "101", "(BODY[])")[0] == "OK"
            assert b"\\Seen" in client.uid("FETCH", "101", "(FLAGS)")[1][0]


def check_empty_mailbox():
    with ImapFixture(raw=None) as server:
        context = ssl.create_default_context(cafile=str(server.ca_file))
        with imaplib.IMAP4_SSL("127.0.0.1", server.port, ssl_context=context, timeout=5) as client:
            client.login(TEST_USER, TEST_PASSWORD)
            assert client.select("INBOX")[1] == [b"0"]
            assert client.uid("SEARCH", None, "ALL")[1] == [b""]


if __name__ == "__main__":
    check_mailbox(implicit_tls=True)
    check_mailbox(implicit_tls=False)
    check_empty_mailbox()
