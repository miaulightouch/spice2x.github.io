from .connection import Connection
from .request import Request

def image_resize_enable(con: Connection, enable: bool):
    req = Request("resize", "image_resize_enable")
    req.add_param(enable)
    con.request(req)

def image_resize_set_scene(con: Connection, scene: int):
    req = Request("resize", "image_resize_set_scene")
    req.add_param(scene)
    con.request(req)


def image_resize_get(con: Connection):
    """Return the enabled state, active scene, filter, and all four scenes."""
    res = con.request(Request("resize", "image_resize_get"))
    return res.get_data()[0]


def image_resize_set(con: Connection, settings: dict):
    """Apply a partial image settings update and return the resulting state."""
    req = Request("resize", "image_resize_set")
    req.add_param(settings)
    res = con.request(req)
    return res.get_data()[0]


def image_resize_save(con: Connection):
    """Save image settings to disk. Requires an API password."""
    con.request(Request("resize", "image_resize_save"))
