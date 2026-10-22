import select
import socket
import struct
import sys
import time

ON_PORT = 5000
MSG_CONNECT = 1
MSG_LINKSTATE = 2
TUPLE_SIZE = 9
LSP_INTERVAL = 10
GRAPH_PRINT_INTERVAL = 10
MAX_NODES = 26


def recv_all(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def parse_link_state(body):
    if len(body) % TUPLE_SIZE != 0 or len(body) > MAX_NODES * TUPLE_SIZE:
        return None
    tuples = []
    for off in range(0, len(body), TUPLE_SIZE):
        letter = chr(body[off])
        if not "A" <= letter <= "Z":
            return None
        ip = socket.inet_ntoa(body[off + 1:off + 5])
        port, cost = struct.unpack("!HH", body[off + 5:off + 9])
        tuples.append((letter, ip, port, cost))
    return tuples


def build_lsp(origin, seq, neighbours):
    pkt = struct.pack("!cIB", origin.encode(), seq, len(neighbours))
    for letter, _ip, _port, cost in neighbours:
        pkt += struct.pack("!cH", letter.encode(), cost)
    return pkt


def parse_lsp(data):
    if len(data) < 6:
        return None
    origin_byte, seq, count = struct.unpack("!cIB", data[:6])
    if len(data) != 6 + 3 * count or count >= MAX_NODES:
        return None
    if not ord("A") <= origin_byte[0] <= ord("Z"):
        return None
    origin = origin_byte.decode()
    links = []
    seen = set()
    for k in range(count):
        nid_byte, cost = struct.unpack("!cH", data[6 + 3 * k:9 + 3 * k])
        if not ord("A") <= nid_byte[0] <= ord("Z"):
            return None
        nid = nid_byte.decode()
        if nid == origin or cost == 0 or nid in seen:
            return None
        seen.add(nid)
        links.append((nid, cost))
    return origin, seq, links


def fmt_links(origin, links):
    return origin + ": " + ",".join(f"{n}={c}" for n, c in links)


def valid_ip(address):
    try:
        packed = socket.inet_aton(address)
    except OSError:
        return False
    value = struct.unpack("!I", packed)[0]
    return value != 0 and (value >> 24) != 127


if len(sys.argv) != 4:
    print(f"Usage: {sys.argv[0]} <ON_IP> <my_IP> <my_UDP_port>")
    sys.exit(1)

on_ip = sys.argv[1]
my_ip = sys.argv[2]
try:
    my_port = int(sys.argv[3])
except ValueError:
    print("UDP port must be an integer.")
    sys.exit(1)
if not valid_ip(on_ip) or not valid_ip(my_ip):
    print("ON and VN IP addresses must be valid, non-loopback IPv4 addresses.")
    sys.exit(1)
if not 1 <= my_port <= 65535:
    print("UDP port must be between 1 and 65535.")
    sys.exit(1)

udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    udp.bind((my_ip, my_port))
except OSError as e:
    print(f"Cannot bind UDP {my_ip}:{my_port}: {e}")
    sys.exit(1)
print(f"UDP socket bound to {my_ip}:{my_port}")

tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    tcp.connect((on_ip, ON_PORT))
except OSError as e:
    print(f"Cannot connect to ON {on_ip}:{ON_PORT}: {e}")
    udp.close()
    sys.exit(1)
print("Connected to Oracle Node.")

body = socket.inet_aton(my_ip) + struct.pack("!H", my_port)
tcp.sendall(struct.pack("!BH", MSG_CONNECT, len(body)) + body)
print(f"Sent CONNECT ({my_ip}, {my_port})")

my_id = None
neighbours = []
seq = 0
lsdb = {}
next_send = None
next_graph_print = None


def send_lsp(destinations=None):
    global seq
    seq += 1
    pkt = build_lsp(my_id, seq, neighbours)
    links = [(node[0], node[3]) for node in neighbours]
    lsdb[my_id] = (seq, links)
    if destinations is None:
        destinations = neighbours
    sent_to = set()
    for letter, ip, port, _cost in destinations:
        endpoint = (ip, port)
        if endpoint in sent_to:
            continue
        sent_to.add(endpoint)
        try:
            udp.sendto(pkt, endpoint)
        except OSError as e:
            print(f"Could not send LSP to {letter} ({ip}:{port}): {e}")
    print(f"Sent LSP seq={seq} to {','.join(sorted({n[0] for n in destinations}))}")


def handle_link_state():
    global my_id, neighbours, next_send, next_graph_print
    hdr = recv_all(tcp, 3)
    if hdr is None:
        return False
    msg_type, length = struct.unpack("!BH", hdr)
    body = recv_all(tcp, length) if length > 0 else b""
    if body is None:
        return False

    if msg_type != MSG_LINKSTATE:
        print(f"Ignoring unknown message type {msg_type}")
        return True

    tuples = parse_link_state(body)
    if tuples is None:
        print("Malformed LINK-STATE body, ignored")
        return True
    me = [t for t in tuples if t[3] == 0]
    if len(me) != 1:
        print("Malformed LINK-STATE: expected exactly one zero-cost tuple")
        return True

    new_id = me[0][0]
    new_neighbours = [t for t in tuples if t[3] != 0]
    if any(t[3] == 0 or t[0] == new_id for t in new_neighbours):
        print("Malformed LINK-STATE: invalid neighbour tuple")
        return True
    if len({t[0] for t in tuples}) != len(tuples):
        print("Malformed LINK-STATE: duplicate node IDs")
        return True
    if my_id is not None and new_id != my_id:
        print("Malformed LINK-STATE: ON changed this node's identity")
        return True

    old_neighbours = neighbours
    my_id = new_id
    neighbours = new_neighbours
    parts = [f"{my_id}=0"] + [f"{t[0]}={t[3]}" for t in neighbours]
    print("LINK-STATE:", ",".join(parts))

    # Notify removed neighbours too, so they can replace stale link-state data.
    send_lsp(old_neighbours + neighbours)
    now = time.monotonic()
    if next_send is None:
        next_send = now + LSP_INTERVAL
    if next_graph_print is None:
        next_graph_print = now + GRAPH_PRINT_INTERVAL
    return True


def handle_lsp():
    data, addr = udp.recvfrom(1024)
    lsp = parse_lsp(data)
    if lsp is None:
        print(f"Malformed LSP from {addr}, ignored")
        return
    origin, rseq, links = lsp
    if origin == my_id:
        return
    if origin not in lsdb or rseq > lsdb[origin][0]:
        lsdb[origin] = (rseq, links)
        print(f"Received LSP seq={rseq}  {fmt_links(origin, links)}")
        pkt = data
        for letter, ip, port, _cost in neighbours:
            if (ip, port) == addr:
                continue
            try:
                udp.sendto(pkt, (ip, port))
            except OSError as e:
                print(f"Could not forward LSP from {origin} to {letter} ({ip}:{port}): {e}")


def print_known_graph():
    print("Known link-state graph:")
    for origin in sorted(lsdb):
        print(fmt_links(origin, lsdb[origin][1]))


try:
    while True:
        deadlines = [time.monotonic() + 1.0]
        if next_send is not None:
            deadlines.append(next_send)
        if next_graph_print is not None:
            deadlines.append(next_graph_print)
        timeout = max(0.0, min(deadlines) - time.monotonic())

        readable, _, _ = select.select([tcp, udp], [], [], timeout)

        if tcp in readable:
            if not handle_link_state():
                print("Oracle Node closed the connection.")
                break
        if udp in readable:
            handle_lsp()

        now = time.monotonic()
        if next_send is not None and now >= next_send:
            send_lsp()
            next_send = now + LSP_INTERVAL
        if next_graph_print is not None and now >= next_graph_print:
            print_known_graph()
            next_graph_print = now + GRAPH_PRINT_INTERVAL

except KeyboardInterrupt:
    print("\nExiting.")
finally:
    tcp.close()
    udp.close()