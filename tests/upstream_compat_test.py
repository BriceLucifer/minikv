#!/usr/bin/env python3
import binascii
import hashlib
import os
import socket
import sys
import time
import unittest
from urllib.parse import quote_plus


def optional_import(name):
    try:
        return __import__(name)
    except ModuleNotFoundError as exc:
        if exc.name == name:
            return None
        raise


requests = optional_import("requests")


def endpoint():
    return os.environ.get("MINIKV_HTTP_ENDPOINT", "http://127.0.0.1:3000")


def fresh_key():
    return endpoint().encode("utf-8") + b"/swag-" + binascii.hexlify(os.urandom(10))


def print_dependency_status():
    print(f"requests: {'available' if requests is not None else 'missing'}")


def require_dependency_status():
    if requests is None:
        raise RuntimeError("missing required HTTP compatibility dependency: requests")


@unittest.skipIf(requests is None, "requests is not installed")
class TestMiniKeyValue(unittest.TestCase):
    maxDiff = None

    def test_getputdelete(self):
        key = fresh_key()

        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

        r = requests.get(key)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.text, "onyou")

        r = requests.delete(key)
        self.assertEqual(r.status_code, 204)

    def test_deleteworks(self):
        key = fresh_key()

        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

        r = requests.delete(key)
        self.assertEqual(r.status_code, 204)

        r = requests.get(key)
        self.assertEqual(r.status_code, 404)

    def test_doubledelete(self):
        key = fresh_key()
        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

        r = requests.delete(key)
        self.assertEqual(r.status_code, 204)

        r = requests.delete(key)
        self.assertNotEqual(r.status_code, 204)

    def test_doubleput(self):
        key = fresh_key()
        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

        r = requests.put(key, data="onyou")
        self.assertNotEqual(r.status_code, 201)

    def test_doubleput_after_delete(self):
        key = fresh_key()
        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

        r = requests.delete(key)
        self.assertEqual(r.status_code, 204)

        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

    def test_10keys(self):
        keys = [fresh_key() for _ in range(10)]

        for key in keys:
            r = requests.put(key, data=hashlib.md5(key).hexdigest())
            self.assertEqual(r.status_code, 201)

        for key in keys:
            r = requests.get(key)
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.text, hashlib.md5(key).hexdigest())

        for key in keys:
            r = requests.delete(key)
            self.assertEqual(r.status_code, 204)

    def test_range_request(self):
        key = fresh_key()
        r = requests.put(key, data="onyou")
        self.assertEqual(r.status_code, 201)

        r = requests.get(key, headers={"Range": "bytes=2-5"})
        self.assertEqual(r.status_code, 206)
        self.assertEqual(r.text, "you")

    def test_nonexistent_key(self):
        key = fresh_key()
        r = requests.get(key)
        self.assertEqual(r.status_code, 404)

    def test_head_request(self):
        key = fresh_key()
        r = requests.head(key, allow_redirects=True)
        self.assertEqual(r.status_code, 404)
        self.assertEqual(int(r.headers["content-length"]), 0)

        key = fresh_key()
        data = "onyou"
        r = requests.put(key, data=data)
        self.assertEqual(r.status_code, 201)

        r = requests.head(key, allow_redirects=True)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(int(r.headers["content-length"]), len(data))

    def test_large_key(self):
        key = fresh_key()
        data = b"a" * (16 * 1024 * 1024)

        r = requests.put(key, data=data)
        self.assertEqual(r.status_code, 201)

        r = requests.get(key)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.content, data)

        r = requests.delete(key)
        self.assertEqual(r.status_code, 204)

    def test_json_list(self):
        key = fresh_key()
        data = "eh"
        r = requests.put(key + b"1", data=data)
        self.assertEqual(r.status_code, 201)
        r = requests.put(key + b"2", data=data)
        self.assertEqual(r.status_code, 201)

        r = requests.get(key + b"?list")
        self.assertEqual(r.status_code, 200)
        base_key = key.decode("utf-8")
        base_key = "/" + base_key.split("/")[-1]
        self.assertEqual(r.json(), {"next": "", "keys": [base_key + "1", base_key + "2"]})

    def test_json_list_null(self):
        r = requests.get(fresh_key() + b"/DOES_NOT_EXIST?list")
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json(), {"next": "", "keys": []})

    def test_json_list_limit(self):
        prefix = fresh_key()
        keys = []
        data = "0"
        limit = 10
        for i in range(limit + 2):
            key = prefix + str(i).encode("utf-8")
            r = requests.put(key, data=data)
            self.assertEqual(r.status_code, 201)
            keys.append("/" + key.decode("utf-8").split("/")[-1])

        keys = sorted(keys)
        r = requests.get(prefix + b"?list&limit=" + str(limit).encode("utf-8"))
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json(), {"next": keys[limit], "keys": keys[:limit]})

        start = quote_plus(r.json()["next"]).encode("utf-8")
        r = requests.get(prefix + b"?list&limit=" + str(limit).encode("utf-8") + b"&start=" + start)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json(), {"next": "", "keys": keys[limit:]})

    def test_noemptykey(self):
        key = fresh_key()
        r = requests.put(key, data="")
        self.assertEqual(r.status_code, 411)

    def test_content_hash(self):
        for _ in range(100):
            key = fresh_key()
            r = requests.put(key, data=key)
            self.assertEqual(r.status_code, 201)

            r = requests.head(key, allow_redirects=False)
            self.assertEqual(r.headers["Content-Md5"], hashlib.md5(key).hexdigest())


if __name__ == "__main__":
    if "--check-deps" in sys.argv:
        print_dependency_status()
        sys.exit(0)
    if "--require-deps" in sys.argv:
        require_dependency_status()
        sys.exit(0)
    if os.environ.get("MINIKV_REQUIRE_HTTP_COMPAT_DEPS") == "1":
        require_dependency_status()

    for port in os.environ.get("MINIKV_HTTP_WAIT_PORTS", "").split(","):
        if not port:
            continue
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", int(port)), timeout=0.5)
                sock.close()
                break
            except OSError:
                time.sleep(0.5)

    unittest.main(verbosity=2)
