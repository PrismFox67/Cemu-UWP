"""#954 PC test for the phone companion: plays DanP's companion app against Cemu_Host.

Start Cemu_Host with CEMU_HOST_TEST_COMPANION=127.0.0.1, then run this script. It acts as the app's DSU server on UDP
26760: waits for the state packet (0x140000), fetches /catalogue and the first game's media from TCP 26761, sends an
arrow key (0x120000), a selection (0x180006) and a launch request (0x180004) and checks the launch status (0x180005).
Pass --no-launch to skip starting the game.
"""
import json, socket, struct, sys, time, urllib.request, zlib

PORT = 26760
HDR = 20


def packet(msg_type, payload):
    p = bytearray(b"DSUS" + struct.pack("<HHII", 1001, len(payload) + 4, 0, 0x12345678) + struct.pack("<I", msg_type) + payload)
    struct.pack_into("<I", p, 8, zlib.crc32(bytes(p)) & 0xFFFFFFFF)
    return bytes(p)


def check_crc(d):
    c = bytearray(d)
    crc = struct.unpack_from("<I", c, 8)[0]
    struct.pack_into("<I", c, 8, 0)
    return zlib.crc32(bytes(c)) & 0xFFFFFFFF == crc


def recv_type(sock, want, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        sock.settimeout(max(0.1, end - time.time()))
        try:
            d, addr = sock.recvfrom(4096)
        except socket.timeout:
            break
        if len(d) >= HDR and d[:4] == b"DSUC" and struct.unpack_from("<I", d, 16)[0] == want:
            assert check_crc(d), "bad CRC"
            return d, addr
    raise SystemExit(f"FAIL: no packet 0x{want:x}")


def main():
    launch = "--no-launch" not in sys.argv
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", PORT))
    d, emu = recv_type(s, 0x140000)
    p = d[HDR:]
    caps = struct.unpack_from("<I", p, 257)[0]
    print(f"state: loaded={p[0]} title={p[1:65].split(b'\0')[0].decode()!r} platform={p[65:129].split(b'\0')[0].decode()!r} caps=0x{caps:x}")
    assert p[65:81] == b"Nintendo - Wii U" and caps & (1 << 13)

    end = time.time() + 30  # the launcher scans the game folders after start
    while True:
        cat = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{PORT + 1}/catalogue", timeout=5).read())
        entries = cat["playlists"][0]["entries"]
        if entries or time.time() > end:
            break
        time.sleep(1)
    print(f"catalogue: {len(entries)} entries, revision {entries[0]['launch_revision'] if entries else '-'}")
    for e in entries:
        print(f"  {e['launch_token']} {e['label']} media=0x{e['media']:x}")
    assert entries, "empty catalogue"
    e = entries[0]
    rev, tok = e["launch_revision"], int(e["launch_token"], 16)
    for mt in (0, 1):
        if e["media"] & (1 << mt):
            r = urllib.request.urlopen(f"http://127.0.0.1:{PORT + 1}/media/{mt}?revision={rev}&token={e['launch_token']}", timeout=5)
            body = r.read()
            print(f"media {mt}: {r.headers['Content-Type']} {len(body)} bytes")
            assert body[:4] in (b"\x89PNG", b"\xff\xd8\xff\xe0", b"\xff\xd8\xff\xe1", b"\xff\xd8\xff\xdb")
    try:
        urllib.request.urlopen(f"http://127.0.0.1:{PORT + 1}/media/1?revision={rev}&token=0000000000000001", timeout=5)
        raise SystemExit("FAIL: unknown token served")
    except urllib.error.HTTPError as err:
        assert err.code == 404

    # right arrow (SDL 275) down then up
    keys = bytearray(51)
    struct.pack_into("<I", keys, 7 + (275 // 32) * 4, 1 << (275 % 32))
    s.sendto(packet(0x120000, bytes(keys)), emu)
    time.sleep(0.3)
    s.sendto(packet(0x120000, bytes(51)), emu)
    if len(entries) > 1:
        d, _ = recv_type(s, 0x180006)
        print(f"selection after right arrow: {struct.unpack_from('<Q', d, HDR + 4)[0]:016X}")
    s.sendto(packet(0x180006, struct.pack("<IQ", rev, tok)), emu)

    s.sendto(packet(0x180004, struct.pack("<IQ", rev + 1, tok)), emu)
    d, _ = recv_type(s, 0x180005)
    assert struct.unpack_from("<I", d, HDR)[0] == 1, "stale revision not refused"
    print("stale revision -> 1 (refused)")
    if launch:
        s.sendto(packet(0x180004, struct.pack("<IQ", rev, tok)), emu)
        d, _ = recv_type(s, 0x180005)
        st = struct.unpack_from("<I", d, HDR)[0]
        print(f"launch {e['label']} -> status {st} {d[HDR + 16:HDR + 144].split(b'\0')[0].decode()!r}")
        assert st == 0
        end = time.time() + 90
        d, _ = recv_type(s, 0x140000)
        while d[HDR] == 0:
            if time.time() > end:
                raise SystemExit("FAIL: game didn't start within 90 s")
            d, _ = recv_type(s, 0x140000)
        print(f"state: loaded=1 title={d[HDR + 1:HDR + 65].split(b'\0')[0].decode()!r}")
    print("PASS")


if __name__ == "__main__":
    main()
