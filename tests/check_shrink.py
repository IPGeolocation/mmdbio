"""shrink against files built by hand (tests/mmdb_py.py): every pointer
width, pointers into the middle of records, map keys behind pointers, data
nothing points at, metadata keys of its own, and an IPv6 subtree shaped
exactly like the IPv4 one. Lookups are compared with an independent reader.

    python3 check_shrink.py path/to/mmdbio workdir
"""
import ipaddress
import os
import random
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mmdb_py import Reader, enc_array, enc_map, enc_ptr, enc_str, enc_value, write_mmdb  # noqa: E402

M = os.path.abspath(sys.argv[1])
work = sys.argv[2]
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
os.chdir(work)
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("PASS ", name)
    else:
        failed += 1
        print("FAIL ", name, detail)


def run(*args):
    p = subprocess.run([M, *args], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def pointer_db(path, ip_version=4, record_size=28):
    """Returns the networks and their expected records."""
    d = bytearray()

    def put(b):
        off = len(d)
        d.extend(b)
        return off

    # As writers lay data out, every top-level value is a record the tree
    # points at; values shared between records sit inside the first one.
    put(enc_str("x" * 3000))                           # unreached
    m_off = put(enc_value({"n": 7, "name": "shared"}))
    name_key = m_off + 1 + len(enc_value(7)) + len(enc_str("n"))  # the "name" key inside M
    assert bytes(d[name_key:name_key + 5]) == enc_str("name")
    put(enc_str("y" * 600000))                         # unreached; pushes what follows past 526335
    alpha, deep = enc_str("alpha"), enc_str("a deep string")
    r1 = len(d)
    r1_body = enc_map([
        (enc_str("a"), alpha),
        (enc_str("deep"), deep),
        (enc_str("shared"), enc_ptr(m_off)),
    ])
    put(r1_body)
    alpha_off = r1 + r1_body.index(alpha)              # targets in the middle of R1
    deep_off = r1 + r1_body.index(deep)
    r2 = put(enc_map([
        (enc_str("x"), enc_ptr(alpha_off)),
        (enc_str("y"), enc_ptr(m_off, 5)),             # wider than it needs
        (enc_str("z"), enc_ptr(deep_off, 5)),
    ]))
    put(enc_str("z" * 70000))                          # unreached, between records
    r3 = put(enc_map([
        (enc_str("list"), enc_array([enc_ptr(r1), enc_ptr(r2), enc_value(1.5)])),
        (enc_ptr(name_key), enc_str("key behind a pointer")),
    ]))
    r4 = put(enc_value({"big": "w" * 300, "n": 1 << 40, "neg": False}))
    put(enc_str("trailing junk"))
    want = {
        r1: {"a": "alpha", "deep": "a deep string", "shared": {"n": 7, "name": "shared"}},
        r2: {"x": "alpha", "y": {"n": 7, "name": "shared"}, "z": "a deep string"},
        m_off: {"n": 7, "name": "shared"},
    }
    want[r3] = {"list": [want[r1], want[r2], 1.5], "name": "key behind a pointer"}
    want[r4] = {"big": "w" * 300, "n": 1 << 40, "neg": False}
    nets = [
        ("1.0.0.0/24", r1), ("2.0.0.0/16", r2), ("3.3.3.3/32", r3), ("4.0.0.0/8", r1), ("5.0.0.0/24", r4),
        ("6.0.0.0/24", m_off),
        # identical subtrees, to be stored once
        ("10.0.0.0/25", r1), ("10.0.0.128/25", r2), ("11.0.0.0/25", r1), ("11.0.0.128/25", r2),
        ("12.0.0.0/25", r1), ("12.0.0.128/25", r2),
    ]
    if ip_version == 6:
        # the same shape as the IPv4 subtree, which must stay a node of its own
        nets += [("2a00::/96", 0)]
        nets = [(n, o) for n, o in nets if n != "2a00::/96"]
        for n, o in list(nets):
            net = ipaddress.ip_network(n)
            if net.version == 4:
                v6 = ipaddress.IPv6Network((int(net.network_address) | 0x2a00 << 112, net.prefixlen + 96))
                nets.append((str(v6), o))
        nets.append(("2a01::/32", r4))
    write_mmdb(path, nets, bytes(d), ip_version=ip_version, record_size=record_size,
               meta_extra={"custom": {"k": [1, 2]}})
    return nets, want


def probes(nets, ip_version):
    rnd = random.Random(5)
    out = []
    for n, _ in nets:
        net = ipaddress.ip_network(n)
        out += [net.network_address, net.broadcast_address, net.network_address + net.num_addresses // 2]
        if net.network_address > ipaddress.ip_address(0 if net.version == 4 else "::"):
            out.append(net.network_address - 1)
    out += [ipaddress.IPv4Address(rnd.getrandbits(32)) for _ in range(3000)]
    out += [ipaddress.IPv4Address(rnd.getrandbits(8) << 24 | rnd.getrandbits(24)) for _ in range(2000)]
    if ip_version == 6:
        out += [ipaddress.IPv6Address(0x2a00 << 112 | rnd.getrandbits(32)) for _ in range(3000)]
        out += [ipaddress.IPv6Address(rnd.getrandbits(128)) for _ in range(500)]
    return [str(a) for a in out]


for ipv, rs in ((4, 28), (6, 32), (4, 24)):
    tag = f"ipv{ipv}-rs{rs}"
    nets, want = pointer_db(f"{tag}.mmdb", ipv, rs)
    a = Reader(f"{tag}.mmdb")
    for n, off in nets:
        if off in want:
            got = a.lookup(str(ipaddress.ip_network(n).network_address))
            assert got == want[off], (n, got)
    rc, out = run("shrink", f"{tag}.mmdb", f"{tag}.small.mmdb", "--full-check", "--lookups", "20000", "--overwrite")
    check(f"{tag}: shrink exits 0", rc == 0, out)
    if rc:
        continue
    b = Reader(f"{tag}.small.mmdb")
    bad = [ip for ip in probes(nets, ipv) if a.lookup(ip) != b.lookup(ip)]
    check(f"{tag}: the independent reader sees the same records", not bad, bad[:5])
    check(f"{tag}: unreached data is dropped", len(b.data) < 2000 < len(a.data), (len(a.data), len(b.data)))
    check(f"{tag}: fewer nodes", b.nodes < a.nodes, (a.nodes, b.nodes))
    meta_a = {k: v for k, v in a.meta.items() if k not in ("node_count", "record_size")}
    meta_b = {k: v for k, v in b.meta.items() if k not in ("node_count", "record_size")}
    check(f"{tag}: the metadata is kept", meta_a == meta_b and b.meta["record_size"] == rs, (meta_a, meta_b))
    rc, out = run("verify", "--db", f"{tag}.mmdb")
    check(f"{tag}: the input fails verify (data nothing points at)", rc == 1 and "does not point to" in out, out)
    rc, out = run("verify", "--db", f"{tag}.small.mmdb")
    check(f"{tag}: the shrunk file passes verify", rc == 0 and out.strip() == "valid", out)
    rc, out = run("shrink", f"{tag}.small.mmdb", f"{tag}.again.mmdb", "--lookups", "1000", "--overwrite")
    c = Reader(f"{tag}.again.mmdb") if rc == 0 else None
    check(f"{tag}: shrinking again changes nothing", c and c.nodes == b.nodes and c.data == b.data, out)
    rc, out = run("shrink", f"{tag}.mmdb", f"{tag}.nc.mmdb", "--no-compact", "--lookups", "1000", "--overwrite")
    d = Reader(f"{tag}.nc.mmdb") if rc == 0 else None
    check(f"{tag}: --no-compact keeps the data section", d and d.data == a.data, out)
    if ipv == 6:
        rc, out = run("export", "--db", f"{tag}.small.mmdb", "--format", "cidr", "--aggregate=false", "--out", "-")
        rc2, out2 = run("export", "--db", f"{tag}.mmdb", "--format", "cidr", "--aggregate=false", "--out", "-")
        check(f"{tag}: export lists the IPv6 twin of the IPv4 subtree", "2a00::100:0/120" in out and
              out.splitlines()[:-1] == out2.splitlines()[:-1], out)

# the record size: down when it fits, an error naming the smallest when not
rc, out = run("shrink", "ipv4-rs28.mmdb", "rs24.mmdb", "--record-size", "24", "--lookups", "1000", "--overwrite")
check("--record-size 24", rc == 0 and Reader("rs24.mmdb").rs == 24, out)
# 17 MB of unreached data: record size 28 until it is dropped
big = bytearray(enc_str("q" * 9_000_000) + enc_str("r" * 9_000_000))  # a value holds at most 16.8 MB
rec = len(big)
big += enc_value({"k": "v"})
write_mmdb("big.mmdb", [("8.8.8.0/24", rec)], bytes(big), record_size=28)
rc, out = run("shrink", "big.mmdb", "big24.mmdb", "--record-size", "24", "--lookups", "1000", "--no-compact")
check("--record-size too small fails and names the smallest", rc == 1 and "needs record size 28" in out, out)
rc, out = run("shrink", "big.mmdb", "big24.mmdb", "--record-size", "24", "--lookups", "1000")
check("compaction makes room for record size 24", rc == 0 and Reader("big24.mmdb").lookup("8.8.8.8") == {"k": "v"},
      out)
rc, out = run("shrink", "ipv4-rs28.mmdb", "--dry-run")
check("--dry-run writes nothing", rc == 0 and "dry run" in out and not os.path.exists("ipv4-rs28.shrunk.mmdb"), out)
rc, out = run("shrink", "ipv4-rs28.mmdb", "rs24.mmdb", "--lookups", "10")
check("an existing output needs --overwrite", rc == 1 and "--overwrite" in out, out)
rc, out = run("shrink", "ipv4-rs28.mmdb", "--lookups", "10")
check("the default output name", rc == 0 and os.path.exists("ipv4-rs28.shrunk.mmdb"), out)
rc, out = run("shrink", "nope.mmdb")
check("a missing input exits 1", rc == 1 and "nope.mmdb" in out, out)

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
