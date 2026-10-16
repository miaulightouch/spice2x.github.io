import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "resources" / "python"))

from spiceapi.info import info_avs, info_log
from spiceapi.request import Request


class FakeResponse:
    def __init__(self, data):
        self.data = data

    def get_data(self):
        return self.data


class FakeConnection:
    def __init__(self):
        self.requests = []

    def request(self, request):
        self.requests.append(request)
        if request.data["function"] == "log":
            return FakeResponse([{"offset": 12, "next": 14, "size": 14, "data": "eHg="}])
        return FakeResponse([{"model": "test"}])


class InfoLogTests(unittest.TestCase):
    def setUp(self):
        Request.GLOBAL_ID = 1

    def test_log_params_do_not_become_request_id(self):
        connection = FakeConnection()

        self.assertEqual(info_log(connection, offset=12, limit=34), {
            "offset": 12,
            "next": 14,
            "size": 14,
            "data": "eHg=",
        })
        log_request = connection.requests[0]
        self.assertEqual(log_request.data["params"], [12, 34])
        self.assertIs(type(log_request.get_id()), int)
        self.assertEqual(Request.GLOBAL_ID, log_request.get_id())

        self.assertEqual(info_avs(connection), {"model": "test"})
        next_request = connection.requests[1]
        self.assertIs(type(next_request.get_id()), int)
        self.assertEqual(next_request.get_id(), log_request.get_id() + 1)


if __name__ == "__main__":
    unittest.main()
