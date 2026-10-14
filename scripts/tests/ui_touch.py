#!/usr/bin/env python3
"""Gives real touches to the mangOS UI and reads its state, through the browser's command port.

Runs on the phone (standard library only). The browser opens its command port only while the file
/etc/mangos/web-test exists: create it, restart mangos-web, and remove it and restart again when
done. Coordinates are in page points (`size` tells how big it is).

  ui_touch.py size                      page width and height
  ui_touch.py tap <x> <y> [ms]          puts the finger down, holds it (80 ms by default) and lifts it
  ui_touch.py tap-on <selector> [ms]    the same, at the center of the element
  ui_touch.py swipe <x1> <y1> <x2> <y2> [ms]
  ui_touch.py eval "<expression>"       evaluates JavaScript in the page and prints the result (JSON)
  ui_touch.py wait <seconds>

Several steps in one command, separated by "then":  ui_touch.py tap-on "#compose" then eval "document.body.className"
"""
import base64, json, os, socket, struct, sys, time, urllib.request

PORT = 9222


class Page:
    def __init__(self):
        try:
            tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % PORT, timeout=3))
        except OSError as e:
            sys.exit("ui_touch: the browser has no command port (%s): needs /etc/mangos/web-test" % e)
        pages = [t for t in tabs if t["type"] == "page" and ":8098" in t["url"]]
        if not pages:
            sys.exit("ui_touch: the UI is not among the browser's pages")
        path = pages[0]["webSocketDebuggerUrl"].split("%d" % PORT, 1)[1]
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, PORT, key)).encode())
        reply = b""
        while b"\r\n\r\n" not in reply:
            reply += self._read(1)
        if b" 101 " not in reply.split(b"\r\n", 1)[0]:
            sys.exit("ui_touch: the browser refuses the connection: %r" % reply[:80])
        self.n = 0

    def _read(self, count):
        data = b""
        while len(data) < count:
            chunk = self.s.recv(count - len(data))
            if not chunk:
                sys.exit("ui_touch: the browser closed the connection")
            data += chunk
        return data

    def _message(self):
        text = b""
        while True:
            head = self._read(2)
            length = head[1] & 127
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            text += self._read(length)
            if head[0] & 0x80:
                return json.loads(text)

    def request(self, method, **params):
        self.n += 1
        body = json.dumps({"id": self.n, "method": method, "params": params}).encode()
        mask = os.urandom(4)
        head = bytes([0x81]) + (bytes([0x80 | len(body)]) if len(body) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(body)))
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(body)))
        while True:
            m = self._message()
            if m.get("id") == self.n:
                if "error" in m:
                    sys.exit("ui_touch: %s: %s" % (method, m["error"]["message"]))
                return m["result"]

    def evaluate(self, expression):
        r = self.request("Runtime.evaluate", expression=expression, returnByValue=True)
        if "exceptionDetails" in r:
            sys.exit("ui_touch: error in the page: %s" % r["exceptionDetails"].get("exception", {}).get("description"))
        return r["result"].get("value")

    def finger(self, kind, points):
        self.request("Input.dispatchTouchEvent", type=kind, touchPoints=[{"x": x, "y": y} for x, y in points])

    def tap(self, x, y, ms=80):
        self.finger("touchStart", [(x, y)])
        time.sleep(ms / 1000)
        self.finger("touchEnd", [])

    def swipe(self, x1, y1, x2, y2, ms=300):
        steps = max(2, int(ms / 16))
        self.finger("touchStart", [(x1, y1)])
        for i in range(1, steps + 1):
            time.sleep(ms / 1000 / steps)
            self.finger("touchMove", [(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)])
        self.finger("touchEnd", [])

    def center(self, selector):
        r = self.evaluate("(function(){var e=document.querySelector(%s);if(!e)return null;var r=e.getBoundingClientRect();"
                          "return r.width?[r.left+r.width/2,r.top+r.height/2]:null})()" % json.dumps(selector))
        if not r:
            sys.exit("ui_touch: %s is missing or not visible" % selector)
        return r


def main():
    steps, step = [], []
    for a in sys.argv[1:]:
        if a == "then":
            steps.append(step); step = []
        else:
            step.append(a)
    steps.append(step)
    if not steps[0]:
        sys.exit(__doc__)
    p = Page()
    for what, *arg in steps:
        if what == "size":
            print(json.dumps(p.evaluate("[innerWidth, innerHeight]")))
        elif what == "tap":
            p.tap(float(arg[0]), float(arg[1]), *[float(a) for a in arg[2:3]])
        elif what == "tap-on":
            x, y = p.center(arg[0])
            p.tap(x, y, *[float(a) for a in arg[1:2]])
        elif what == "swipe":
            p.swipe(*[float(a) for a in arg[:5]])
        elif what == "eval":
            print(json.dumps(p.evaluate(arg[0]), ensure_ascii=False))
        elif what == "wait":
            time.sleep(float(arg[0]))
        else:
            sys.exit("ui_touch: unknown command \"%s\"" % what)


if __name__ == "__main__":
    main()
