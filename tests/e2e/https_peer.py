"""Verified HTTPS test peer reached through CONNECT, with no route to real upstreams."""

import http.server
import shutil
import ssl
import subprocess
import threading

import harness


class HttpsPeer:
    def __init__(self, home, domains, origin):
        openssl = shutil.which("openssl")
        if openssl is None:
            harness.skip("openssl is required for the test HTTPS certificate")
        cert, key = home / "cert.pem", home / "key.pem"
        subprocess.run(
            [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", f"/CN={domains[0]}",
             "-addext", "subjectAltName=" + ",".join(f"DNS:{name}" for name in domains),
             "-keyout", str(key), "-out", str(cert)],
            check=True, capture_output=True,
        )
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)

        class Proxy(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_CONNECT(self):
                if self.path not in [f"{name}:443" for name in domains]:
                    self.send_error(502)
                    return
                self.send_response(200)
                self.end_headers()
                self.wfile.flush()
                try:
                    with context.wrap_socket(self.connection, server_side=True) as connection:
                        origin(connection, self.client_address, self.server)
                except (ConnectionResetError, BrokenPipeError):
                    # Exiting hax cancels any background metadata request still in flight.
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
        self.thread = threading.Thread(target=self.server.serve_forever)
        proxy = f"http://127.0.0.1:{self.server.server_port}"
        self.env = {
            "https_proxy": proxy, "HTTPS_PROXY": proxy, "ALL_PROXY": "",
            "no_proxy": "", "NO_PROXY": "", "CURL_CA_BUNDLE": str(cert),
            "HAX_CATALOG_URL": "", "HAX_TRACE": str(home / "trace"),
        }

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
