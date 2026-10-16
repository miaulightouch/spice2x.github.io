from .connection import Connection
from .request import Request


def info_avs(con: Connection):
    res = con.request(Request("info", "avs"))
    return res.get_data()[0]


def info_launcher(con: Connection):
    res = con.request(Request("info", "launcher"))
    return res.get_data()[0]


def info_memory(con: Connection):
    res = con.request(Request("info", "memory"))
    return res.get_data()[0]


def info_log(con: Connection, offset=None, limit=65536):
    """Read a bounded base64 chunk of the active log; None reads the tail."""
    req = Request("info", "log")
    req.add_param(offset)
    req.add_param(limit)
    return con.request(req).get_data()[0]
