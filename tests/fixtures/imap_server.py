"""仅监听本机回环地址的简易 IMAP 测试服务器，不用于实际收信。"""

import argparse
import base64
import binascii
import contextlib
import json
import pathlib
import shlex
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time


TEST_USER = "mailai-test"
TEST_PASSWORD = "fixture-only-password"
SAMPLE_MAIL = (
    "From: sender@example.test\r\n"
    "Subject: IMAP fixture\r\n"
    "Content-Type: text/plain; charset=UTF-8\r\n\r\n"
    "这是一封测试邮件。\r\n"
).encode("utf-8")


class _Server(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, tls, implicit_tls, raw, search_mode, allow_starttls,
                 uid_validity, fetch_mode, search_failures, fetch_failures, connect_failures):
        self.tls = tls
        self.implicit_tls = implicit_tls
        self.raw = raw
        self.search_mode = search_mode
        self.search_failures = search_failures
        self.fetch_failures = fetch_failures
        self.connect_failures = connect_failures
        self.allow_starttls = allow_starttls
        self.search_requests = 0
        self.uid_validity = uid_validity
        self.fetch_mode = fetch_mode
        self.fetch_requests = 0
        self.fetch_items = []
        self.connection_times = []
        self.select_validities = []
        self.failure_times = []
        self.seen = False
        super().__init__(("127.0.0.1", 0), _Handler)


class _Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.transport = self.request
        self.transport.settimeout(5)
        self.reader = None
        self.secure = False
        self.authenticated = False
        self.selected = False
        self.read_only = False
        try:
            self.server.connection_times.append(time.monotonic())
            if len(self.server.connection_times) <= self.server.connect_failures:
                self.server.failure_times.append(time.monotonic())
                return
            if self.server.implicit_tls:
                self._upgrade_tls()
            else:
                self.reader = self.transport.makefile("rb")
            self._send("* OK MailAI test mailbox\r\n")
            while line := self.reader.readline(8192):
                if not line.endswith(b"\r\n") or not self._command(line.decode("ascii").strip()):
                    return
        except (OSError, UnicodeError):
            # 连接关闭、TLS 握手失败和超时都是预期的测试场景。
            return
        finally:
            if self.reader is not None:
                self.reader.close()
            self.transport.close()

    def _upgrade_tls(self):
        if self.reader is not None:
            self.reader.close()
            self.reader = None
        self.transport = self.server.tls.wrap_socket(self.transport, server_side=True)
        self.reader = self.transport.makefile("rb")
        self.secure = True

    def _send(self, message):
        self.transport.sendall(message.encode("ascii"))

    def _command(self, line):
        fields = line.split(maxsplit=2)
        if len(fields) < 2:
            return False
        tag, command = fields[:2]
        arguments = fields[2] if len(fields) == 3 else ""
        command = command.upper()

        if command == "CAPABILITY":
            if self.secure:
                capabilities = "IMAP4rev1 AUTH=PLAIN"
            else:
                capabilities = "IMAP4rev1 LOGINDISABLED"
                if self.server.allow_starttls:
                    capabilities += " STARTTLS"
            self._send(f"* CAPABILITY {capabilities}\r\n{tag} OK CAPABILITY completed\r\n")
        elif command == "LOGOUT":
            self._send(f"* BYE Goodbye\r\n{tag} OK LOGOUT completed\r\n")
            return False
        elif command == "STARTTLS" and not self.secure and self.server.allow_starttls:
            self._send(f"{tag} OK Begin TLS negotiation\r\n")
            self._upgrade_tls()
        elif command in ("LOGIN", "AUTHENTICATE") and self.secure:
            self._authenticate(tag, command, arguments)
        elif not self.authenticated:
            self._send(f"{tag} NO Authentication required\r\n")
        elif command in ("SELECT", "EXAMINE") and arguments.strip('"').upper() == "INBOX":
            self.selected = True
            self.read_only = command == "EXAMINE"
            self.server.select_validities.append(self.server.uid_validity)
            count = int(self.server.raw is not None)
            access = "READ-ONLY" if self.read_only else "READ-WRITE"
            validity = (f"* OK [UIDVALIDITY {self.server.uid_validity}] UIDs valid\r\n"
                        if self.server.uid_validity is not None else "")
            self._send(
                "* FLAGS (\\Seen)\r\n"
                f"* {count} EXISTS\r\n* 0 RECENT\r\n"
                f"{validity}"
                "* OK [UIDNEXT 102] Next UID\r\n"
                f"{tag} OK [{access}] {command} completed\r\n"
            )
        elif command == "LIST":
            self._send(f'* LIST () "/" "INBOX"\r\n{tag} OK LIST completed\r\n')
        elif command == "NOOP":
            self._send(f"{tag} OK NOOP completed\r\n")
        elif self.selected and command == "UID":
            return self._uid_command(tag, arguments)
        else:
            self._send(f"{tag} BAD Unsupported fixture command\r\n")
        return True

    def _authenticate(self, tag, command, arguments):
        valid = False
        if command == "LOGIN":
            try:
                valid = shlex.split(arguments) == [TEST_USER, TEST_PASSWORD]
            except ValueError:
                pass
        elif arguments.upper() == "PLAIN":
            self._send("+ \r\n")
            try:
                fields = base64.b64decode(self.reader.readline(8192).strip(), validate=True).split(b"\0")
                valid = fields == [b"", TEST_USER.encode(), TEST_PASSWORD.encode()]
            except (ValueError, binascii.Error):
                pass
        if valid:
            self.authenticated = True
            self._send(f"{tag} OK Authentication completed\r\n")
        else:
            self._send(f"{tag} NO Authentication failed\r\n")

    def _uid_command(self, tag, arguments):
        upper = arguments.upper()
        if upper == "SEARCH ALL":
            self.server.search_requests += 1
            mode = self.server.search_mode
            if (self.server.search_failures is not None and
                    self.server.search_requests > self.server.search_failures):
                mode = "normal"
            if mode == "rejected":
                self._send(f"{tag} NO Search denied\r\n")
                return True
            if mode == "malformed":
                self._send(f"* SEARCH 0\r\n{tag} OK UID SEARCH completed\r\n")
                return True
            if mode == "unavailable":
                self.server.failure_times.append(time.monotonic())
                self._send(f"{tag} NO [UNAVAILABLE] Temporarily unavailable\r\n")
                return True
            if mode == "disconnect":
                self._send("* SEARCH 999\r\n")
                self.server.failure_times.append(time.monotonic())
                return False
            if mode == "timeout":
                time.sleep(2)
            ids = " 101" if self.server.raw is not None else ""
            self._send(f"* SEARCH{ids}\r\n{tag} OK UID SEARCH completed\r\n")
        elif upper.startswith("FETCH "):
            fields = upper.split(maxsplit=2)
            if len(fields) != 3:
                self._send(f"{tag} BAD Invalid UID FETCH\r\n")
                return True
            self.server.fetch_requests += 1
            self.server.fetch_items.append(fields[2])
            mode = self.server.fetch_mode
            if (self.server.fetch_failures is not None and
                    self.server.fetch_requests > self.server.fetch_failures):
                mode = "normal"
            if mode == "rejected":
                self._send(f"{tag} NO Fetch denied\r\n")
                return True
            if mode == "unavailable":
                self.server.failure_times.append(time.monotonic())
                self._send(f"{tag} NO [UNAVAILABLE] Temporarily unavailable\r\n")
                return True
            if mode == "timeout":
                time.sleep(2)
            if fields[1] == "101" and self.server.raw is not None:
                if not self._fetch(fields[2], mode):
                    return False
            if mode == "unavailable-after-body":
                self.server.failure_times.append(time.monotonic())
                self._send(f"{tag} NO [UNAVAILABLE] Temporarily unavailable\r\n")
                return True
            self._send(f"{tag} OK UID FETCH completed\r\n")
        else:
            self._send(f"{tag} BAD Unsupported fixture UID command\r\n")
        return True

    def _fetch(self, items, mode):
        body = ("BODY.PEEK[]" in items or "BODY[]" in items) and mode != "missing-body"
        if "BODY[]" in items and not self.read_only:
            self.server.seen = True
        fields = ["UID 102" if mode == "wrong-uid" else "UID 101"]
        if "FLAGS" in items:
            flags = "\\Seen" if self.server.seen else ""
            fields.append(f"FLAGS ({flags})")
        if "INTERNALDATE" in items and mode != "missing-date":
            date = "31-Nov-2026 10:00:00 +0800" if mode == "invalid-date" else "21-Nov-2026 10:00:00 +0800"
            fields.append(f'INTERNALDATE "{date}"')
        if "RFC822.SIZE" in items:
            fields.append(f"RFC822.SIZE {len(self.server.raw)}")
        prefix = "* 1 FETCH (" + " ".join(fields)
        if body:
            if mode == "flags-before":
                self._send("* 1 FETCH (UID 101 FLAGS ())\r\n")
            if mode == "body-first":
                self._send(f"* 1 FETCH (BODY[] {{{len(self.server.raw)}}}\r\n")
            else:
                self._send(f"{prefix} BODY[] {{{len(self.server.raw)}}}\r\n")
            if mode in ("disconnect", "disconnect-stale", "disconnect-gone"):
                self.transport.sendall(self.server.raw[:len(self.server.raw) // 2])
                if mode == "disconnect-stale":
                    self.server.uid_validity = 78
                if mode == "disconnect-gone":
                    self.server.raw = None
                self.server.failure_times.append(time.monotonic())
                return False
            self.transport.sendall(self.server.raw)
            if mode != "missing-closure":
                suffix = " " + " ".join(fields) if mode == "body-first" else ""
                self._send(suffix + ")\r\n")
        else:
            self._send(prefix + ")\r\n")
        return True


class ImapFixture:
    """启动本地服务器，并在退出时清理监听资源和测试证书。"""

    def __init__(self, *, implicit_tls=True, raw=SAMPLE_MAIL,
                 certificate_san="DNS:localhost,IP:127.0.0.1",
                 search_mode="normal", allow_starttls=True,
                 uid_validity=77, fetch_mode="normal", search_failures=None,
                 fetch_failures=None, connect_failures=0):
        self.implicit_tls = implicit_tls
        self.raw = raw
        self.certificate_san = certificate_san
        self.search_mode = search_mode
        self.allow_starttls = allow_starttls
        self.uid_validity = uid_validity
        self.fetch_mode = fetch_mode
        self.search_failures = search_failures
        self.fetch_failures = fetch_failures
        self.connect_failures = connect_failures

    def __enter__(self):
        with contextlib.ExitStack() as stack:
            directory = pathlib.Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="mailai-imap-")))
            self.ca_file = directory / "certificate.pem"
            key = directory / "private-key.pem"
            subprocess.run(
                ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-keyout", str(key), "-out", str(self.ca_file), "-subj", "/CN=localhost",
                 "-addext", "subjectAltName=" + self.certificate_san],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(self.ca_file, key)
            self.server = stack.enter_context(_Server(tls, self.implicit_tls, self.raw,
                                                        self.search_mode, self.allow_starttls,
                                                        self.uid_validity, self.fetch_mode,
                                                        self.search_failures, self.fetch_failures,
                                                        self.connect_failures))
            self.port = self.server.server_address[1]
            thread = threading.Thread(target=self.server.serve_forever, daemon=True)
            thread.start()
            stack.callback(thread.join)
            stack.callback(self.server.shutdown)
            self._cleanup = stack.pop_all()
        return self

    def __exit__(self, *exc):
        return self._cleanup.__exit__(*exc)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--starttls", action="store_true")
    args = parser.parse_args()
    with ImapFixture(implicit_tls=not args.starttls) as fixture:
        print(json.dumps({"host": "127.0.0.1", "port": fixture.port,
                          "ca_file": str(fixture.ca_file)}), flush=True)
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass
