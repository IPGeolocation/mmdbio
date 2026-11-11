"""A small, independent MMDB writer and reader for the tests.

The writer lays out the data section by hand, so tests can build what
mmdbio's own writer never makes: pointers of every width (including wider
than needed), pointers into the middle of other records, and data nothing
points at. The reader decodes with no help from mmdbio's code.
"""
import ipaddress
import struct

MARKER = b"\xab\xcd\xefMaxMind.com"


# ---- encoding ----

def ctrl(type_, size):
    if type_ <= 7:
        first, ext = type_ << 5, b""
    else:
        first, ext = 0, bytes([type_ - 7])
    if size < 29:
        return bytes([first | size]) + ext
    if size < 285:
        return bytes([first | 29]) + ext + bytes([size - 29])
    if size < 65821:
        return bytes([first | 30]) + ext + struct.pack(">H", size - 285)
    return bytes([first | 31]) + ext + (size - 65821).to_bytes(3, "big")


def enc_str(s):
    b = s.encode()
    return ctrl(2, len(b)) + b


def enc_uint(type_, v):
    b = v.to_bytes(16, "big").lstrip(b"\0")
    return ctrl(type_, len(b)) + b


def enc_ptr(target, width=None):
    """A pointer of width bytes (2..5); the smallest that fits by default."""
    if width is None:
        width = 2 if target <= 2047 else 3 if target <= 526335 else 4 if target <= 134744063 else 5
    if width == 2:
        assert target <= 2047
        return bytes([0x20 | target >> 8, target & 0xFF])
    if width == 3:
        v = target - 2048
        assert 0 <= v < 1 << 19
        return bytes([0x28 | v >> 16]) + (v & 0xFFFF).to_bytes(2, "big")
    if width == 4:
        v = target - 526336
        assert 0 <= v < 1 << 27
        return bytes([0x30 | v >> 24]) + (v & 0xFFFFFF).to_bytes(3, "big")
    return bytes([0x38]) + target.to_bytes(4, "big")


def enc_map(pairs):
    """pairs: (key bytes, value bytes), each already encoded."""
    out = ctrl(7, len(pairs))
    for k, v in pairs:
        out += k + v
    return out


def enc_array(items):
    out = ctrl(11, len(items))
    for v in items:
        out += v
    return out


def enc_value(v):
    if isinstance(v, bool):
        return ctrl(14, 1 if v else 0)
    if isinstance(v, str):
        return enc_str(v)
    if isinstance(v, int):
        return enc_uint(6 if v < 1 << 32 else 9, v)
    if isinstance(v, float):
        return ctrl(3, 8) + struct.pack(">d", v)
    if isinstance(v, dict):
        return enc_map([(enc_str(k), enc_value(x)) for k, x in sorted(v.items())])
    if isinstance(v, list):
        return enc_array([enc_value(x) for x in v])
    raise TypeError(v)


def write_mmdb(path, networks, data, ip_version=4, record_size=28, meta_extra=None):
    """networks: (cidr, data offset); data: the data section's bytes."""
    root = {}
    width = 32 if ip_version == 4 else 128
    for cidr, off in networks:
        net = ipaddress.ip_network(cidr)
        a = int(net.network_address)
        bits = net.prefixlen
        w = 32 if net.version == 4 else 128
        if ip_version == 6 and net.version == 4:  # IPv4 sits at ::a.b.c.d
            bits += 96
            w = 128
        assert w == width, "an IPv6 network in an IPv4 tree"
        node = root
        for i in range(bits):
            b = a >> (w - 1 - i) & 1
            if i == bits - 1:
                node[b] = ("data", off)
            else:
                nxt = node.get(b)
                if not isinstance(nxt, dict):
                    nxt = {}
                    node[b] = nxt
                node = nxt
    nodes = []
    index = {}

    def number(n):
        index[id(n)] = len(nodes)
        nodes.append(n)
        for b in (0, 1):
            if isinstance(n.get(b), dict):
                number(n[b])

    number(root)
    count = len(nodes)

    def rec(v):
        if v is None:
            return count
        if isinstance(v, tuple):
            return count + 16 + v[1]
        return index[id(v)]

    tree = bytearray()
    for n in nodes:
        l, r = rec(n.get(0)), rec(n.get(1))
        if record_size == 24:
            tree += l.to_bytes(3, "big") + r.to_bytes(3, "big")
        elif record_size == 28:
            tree += (l & 0xFFFFFF).to_bytes(3, "big") + bytes([(l >> 24) << 4 | r >> 24]) + (r & 0xFFFFFF).to_bytes(3, "big")
        else:
            tree += l.to_bytes(4, "big") + r.to_bytes(4, "big")
    meta = {
        "binary_format_major_version": 2,
        "binary_format_minor_version": 0,
        "build_epoch": 1700000000,
        "database_type": "Test-Pointers",
        "description": {"en": "pointer test"},
        "ip_version": ip_version,
        "languages": ["en"],
        "node_count": count,
        "record_size": record_size,
    }
    if meta_extra:
        meta.update(meta_extra)
    mpairs = []
    for k, v in sorted(meta.items()):
        if k in ("binary_format_major_version", "binary_format_minor_version", "ip_version", "record_size"):
            mpairs.append((enc_str(k), enc_uint(5, v)))
        elif k in ("build_epoch",):
            mpairs.append((enc_str(k), enc_uint(9, v)))
        elif k == "node_count":
            mpairs.append((enc_str(k), enc_uint(6, v)))
        else:
            mpairs.append((enc_str(k), enc_value(v)))
    with open(path, "wb") as f:
        f.write(bytes(tree) + b"\0" * 16 + data + MARKER + enc_map(mpairs))
    return count


# ---- decoding ----

class Reader:
    def __init__(self, path):
        self.buf = open(path, "rb").read()
        m = self.buf.rindex(MARKER)
        self.meta, _ = self._decode(self.buf, m + len(MARKER), m + len(MARKER))
        self.nodes = self.meta["node_count"]
        self.rs = self.meta["record_size"]
        tree = self.nodes * self.rs // 4
        self.data_start = tree + 16
        self.data = self.buf[self.data_start:m]

    def _decode(self, b, off, base):
        c = b[off]
        off += 1
        t = c >> 5
        if t == 1:
            s = c >> 3 & 3
            v = int.from_bytes(b[off:off + s + 1], "big")
            vvv = c & 7
            target = [vvv << 8 | v, (vvv << 16 | v) + 2048, (vvv << 24 | v) + 526336, v][s]
            val, _ = self._decode(b, base + target, base)
            return val, off + s + 1
        if t == 0:
            t = 7 + b[off]
            off += 1
        size = c & 0x1F
        if size >= 29:
            n = size - 28
            v = int.from_bytes(b[off:off + n], "big")
            off += n
            size = [29, 285, 65821][n - 1] + v
        if t == 2:
            return b[off:off + size].decode(), off + size
        if t == 3:
            return struct.unpack(">d", b[off:off + 8])[0], off + 8
        if t == 4:
            return b[off:off + size], off + size
        if t in (5, 6, 9, 10):
            return int.from_bytes(b[off:off + size], "big"), off + size
        if t == 8:
            return int.from_bytes(b[off:off + size], "big", signed=size == 4), off + size
        if t == 7:
            out = {}
            for _ in range(size):
                k, off = self._decode(b, off, base)
                v, off = self._decode(b, off, base)
                out[k] = v
            return out, off
        if t == 11:
            out = []
            for _ in range(size):
                v, off = self._decode(b, off, base)
                out.append(v)
            return out, off
        if t == 14:
            return bool(size), off
        if t == 15:
            return struct.unpack(">f", b[off:off + 4])[0], off + 4
        raise ValueError("type %d" % t)

    def node(self, i):
        n = self.rs // 4
        b = self.buf[i * n:(i + 1) * n]
        if self.rs == 24:
            return int.from_bytes(b[:3], "big"), int.from_bytes(b[3:], "big")
        if self.rs == 28:
            return (b[3] >> 4) << 24 | int.from_bytes(b[:3], "big"), (b[3] & 15) << 24 | int.from_bytes(b[4:], "big")
        return int.from_bytes(b[:4], "big"), int.from_bytes(b[4:], "big")

    def lookup(self, ip):
        a = ipaddress.ip_address(ip)
        bits = 32 if a.version == 4 else 128
        v = int(a)
        node = 0
        if self.meta["ip_version"] == 6 and a.version == 4:
            for _ in range(96):
                if node >= self.nodes:
                    break
                node = self.node(node)[0]
        i = 0
        while i < bits and node < self.nodes:
            node = self.node(node)[v >> (bits - 1 - i) & 1]
            i += 1
        if node == self.nodes:
            return None
        return self._decode(self.data, node - self.nodes - 16, 0)[0]
