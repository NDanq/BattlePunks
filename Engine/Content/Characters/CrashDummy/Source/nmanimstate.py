"""Read and write .nmanimstate files (RenderCore/AnimStateAsset.h) from Python.

NAST v1 { APRM v1, ALYR v5, [ALOK v1], [ARTG v1], [AIKS v1] }; EngineCore::Archive
chunks are a little-endian tag, a u16 version and a u32 size. Strings are a u32
length and UTF-8; bools and enums one byte; floats f32; ints i32.
"""
import struct


def _tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


class _Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, fmt):
        values = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += struct.calcsize("<" + fmt)
        return values if len(values) > 1 else values[0]

    def string(self):
        n = self.take("I")
        text = self.data[self.pos:self.pos + n].decode("utf-8")
        self.pos += n
        return text

    def peek_tag(self):
        return struct.unpack_from("<I", self.data, self.pos)[0] if self.pos + 4 <= len(self.data) else None

    def open(self, name, version):
        tag, ver, size = self.take("IHI")
        assert tag == _tag(name), f"expected {name}"
        assert ver == version, f"{name} v{ver}, this reads v{version}"
        return self.pos + size

    def items(self, each):
        return [each() for _ in range(self.take("I"))]


class _Writer:
    def __init__(self):
        self.parts = []

    def put(self, fmt, *values):
        self.parts.append(struct.pack("<" + fmt, *values))

    def string(self, text):
        data = text.encode("utf-8")
        self.put("I", len(data))
        self.parts.append(data)

    def items(self, values, each):
        self.put("I", len(values))
        for value in values:
            each(value)

    def bytes(self):
        return b"".join(self.parts)


def _chunk(name, version, payload):
    return struct.pack("<IHI", _tag(name), version, len(payload)) + payload


def load(path):
    r = _Reader(open(path, "rb").read())
    end = r.open("NAST", 1)
    state = {}
    r.open("APRM", 1)
    state["Parameters"] = r.items(lambda: {"Name": r.string(), "Type": r.take("B"), "Default": r.take("f")})
    r.open("ALYR", 5)

    def sample():
        return {"Clip": r.string(), "Position": r.take("f"), "PositionY": r.take("f")}

    def node():
        s = {"Name": r.string(), "BlendParameter": r.string(), "Speed": r.take("f"), "BlendParameterY": r.string()}
        s["EditorX"], s["EditorY"] = r.take("ff")
        s["Loop"] = r.take("B")
        s["SyncGroup"] = r.string()
        s["Samples"] = r.items(sample)
        return s

    def condition():
        return {"Parameter": r.string(), "Compare": r.take("B"), "Value": r.take("f")}

    def transition():
        t = {"From": r.take("i"), "To": r.take("i"), "Duration": r.take("f"), "AtEnd": bool(r.take("B"))}
        t["Conditions"] = r.items(condition)
        return t

    def layer():
        l = {"Name": r.string(), "Weight": r.take("f"), "WeightParameter": r.string(), "EntryState": r.take("i")}
        l["Blend"], l["AdditiveBase"] = r.take("BB")
        l["MaskBones"] = r.items(r.string)
        l["EditorPlaced"] = bool(r.take("B"))
        l["AnyEditorX"], l["AnyEditorY"] = r.take("ff")
        l["States"] = r.items(node)
        l["Transitions"] = r.items(transition)
        return l

    state["Layers"] = r.items(layer)
    # Everything after the layers (look-at, retarget, IK) is kept as it is.
    state["Tail"] = r.data[r.pos:end]
    return state


def save(path, state):
    w = _Writer()
    w.items(state["Parameters"], lambda p: (w.string(p["Name"]), w.put("Bf", p["Type"], p["Default"])))
    params = w.bytes()

    w = _Writer()

    def sample(s):
        w.string(s["Clip"])
        w.put("ff", s["Position"], s.get("PositionY", 0.0))

    def node(s):
        w.string(s["Name"]); w.string(s.get("BlendParameter", "")); w.put("f", s.get("Speed", 1.0))
        w.string(s.get("BlendParameterY", "")); w.put("ff", s.get("EditorX", 0.0), s.get("EditorY", 0.0))
        w.put("B", s.get("Loop", 0)); w.string(s.get("SyncGroup", ""))
        w.items(s["Samples"], sample)

    def condition(c):
        w.string(c["Parameter"]); w.put("Bf", c["Compare"], c.get("Value", 0.0))

    def transition(t):
        w.put("iifB", t["From"], t["To"], t["Duration"], 1 if t.get("AtEnd") else 0)
        w.items(t.get("Conditions", []), condition)

    def layer(l):
        w.string(l["Name"]); w.put("f", l.get("Weight", 1.0)); w.string(l.get("WeightParameter", ""))
        w.put("iBB", l.get("EntryState", 0), l.get("Blend", 0), l.get("AdditiveBase", 0))
        w.items(l.get("MaskBones", []), w.string)
        w.put("Bff", 1 if l.get("EditorPlaced") else 0, l.get("AnyEditorX", 0.0), l.get("AnyEditorY", 0.0))
        w.items(l["States"], node)
        w.items(l.get("Transitions", []), transition)

    w.items(state["Layers"], layer)
    layers = w.bytes()
    body = _chunk("APRM", 1, params) + _chunk("ALYR", 5, layers) + state.get("Tail", b"")
    open(path, "wb").write(_chunk("NAST", 1, body))
