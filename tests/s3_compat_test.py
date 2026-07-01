#!/usr/bin/env python3
import binascii
import os
import sys
import unittest


def optional_import(name):
    try:
        return __import__(name)
    except ModuleNotFoundError as exc:
        if exc.name == name:
            return None
        raise


boto3 = optional_import("boto3")
pyarrow = optional_import("pyarrow")
if pyarrow is not None:
    import pyarrow.parquet as pq
    from pyarrow import fs


def fresh_key(prefix):
    return prefix + binascii.hexlify(os.urandom(10)).decode("utf-8")


def s3_endpoint():
    return os.environ.get("MINIKV_S3_ENDPOINT", "http://127.0.0.1:3000")


def pyarrow_endpoint_override():
    endpoint = s3_endpoint()
    return endpoint.removeprefix("http://").removeprefix("https://")


def print_dependency_status():
    print(f"boto3: {'available' if boto3 is not None else 'missing'}")
    print(f"pyarrow: {'available' if pyarrow is not None else 'missing'}")


@unittest.skipIf(boto3 is None, "boto3 is not installed")
class TestS3Boto(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s3 = boto3.client(
            "s3",
            endpoint_url=s3_endpoint(),
            aws_access_key_id="user",
            aws_secret_access_key="password",
        )

    def test_writelist_metadata_and_delete(self):
        key = fresh_key("swag-")
        body = b"hello1"

        put = self.s3.put_object(Body=body, Bucket="boto", Key=key)
        self.assertIn("ETag", put)

        listed = self.s3.list_objects_v2(Bucket="boto")
        entries = {item["Key"]: item for item in listed.get("Contents", [])}
        self.assertIn(key, entries)
        self.assertEqual(entries[key]["Size"], len(body))
        self.assertIn("ETag", entries[key])

        self.s3.delete_object(Bucket="boto", Key=key)
        after_delete = self.s3.list_objects_v2(Bucket="boto")
        keys = [item["Key"] for item in after_delete.get("Contents", [])]
        self.assertNotIn(key, keys)

    @unittest.expectedFailure
    def test_writeread_keeps_upstream_redirect_limitation(self):
        key = fresh_key("swag-")
        self.s3.put_object(Body=b"hello1", Bucket="boto", Key=key)
        self.s3.get_object(Bucket="boto", Key=key)


@unittest.skipIf(pyarrow is None, "pyarrow is not installed")
class TestS3PyArrow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ["AWS_EC2_METADATA_DISABLED"] = "true"
        cls.s3 = fs.S3FileSystem(
            endpoint_override=pyarrow_endpoint_override(),
            scheme="http",
            anonymous=True,
        )

    def get_fresh_key(self, suffix):
        return fresh_key("bucket/swag-") + suffix

    def write_file(self, path, data):
        with self.s3.open_output_stream(path) as out:
            out.write(data)

    def test_fileinfo(self):
        path = self.get_fresh_key("-fileinfo")
        self.write_file(path, b"hello1")

        info = self.s3.get_file_info(path)

        self.assertEqual(info.size, 6)
        self.s3.delete_file(path)

    def test_fileinfo_list_and_delete_dir_contents(self):
        path = self.get_fresh_key("-listdir")
        self.write_file(path, b"hello1")

        infos = self.s3.get_file_info(fs.FileSelector("bucket/", recursive=True))
        paths = [item.path for item in infos]
        self.assertIn(path, paths)

        self.s3.delete_dir_contents("bucket")
        info = self.s3.get_file_info(path)
        self.assertEqual(info.type, fs.FileType.NotFound)

    def test_deletefile(self):
        path = self.get_fresh_key("-delftest")
        self.write_file(path, b"hello1")
        self.assertEqual(self.s3.get_file_info(path).size, 6)

        self.s3.delete_file(path)

        info = self.s3.get_file_info(path)
        self.assertEqual(info.type, fs.FileType.NotFound)

    def test_small_parquet_roundtrip(self):
        table = pyarrow.table([pyarrow.array([0, 1, 2, 3])], ["a"])
        path = self.get_fresh_key("-small-parquet")

        pq.write_table(table, path, filesystem=self.s3)
        loaded = pq.read_table(path, filesystem=self.s3)

        self.assertEqual(table, loaded)
        self.s3.delete_file(path)

    @unittest.skipUnless(
        os.environ.get("MINIKV_RUN_LARGE_S3_COMPAT") == "1",
        "set MINIKV_RUN_LARGE_S3_COMPAT=1 to run the large multipart parquet test",
    )
    def test_large_parquet_roundtrip(self):
        table = pyarrow.table(
            [pyarrow.array(range(2_000_000)), pyarrow.array(range(2_000_000))],
            ["a", "b"],
        )
        path = self.get_fresh_key("-large-parquet")

        pq.write_table(table, path, filesystem=self.s3)
        loaded = pq.read_table(path, filesystem=self.s3)

        self.assertEqual(table, loaded)
        self.s3.delete_file(path)


if __name__ == "__main__":
    if "--check-deps" in sys.argv:
        print_dependency_status()
        sys.exit(0)
    unittest.main(verbosity=2)
