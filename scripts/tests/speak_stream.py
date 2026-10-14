#!/usr/bin/env python3
"""Tests streamed speech without playing anything: starts a test mangos-speak service (always
with the local default voice) whose "speaker" is a file, sends it three phrases one second apart
as the agent would while the model writes, and reports when the first sound arrived and how long
it lasted. Then checks that the stop command ends a stream left waiting. Runs on the phone.
"""
import importlib.machinery, importlib.util, json, os, shutil, socket, stat, tempfile, threading, time

work = tempfile.mkdtemp(prefix="speak-stream-")
fake = os.path.join(work, "aplay")
with open(fake, "w") as f:
    f.write("#!/bin/sh\nexec cat > %s/sound.raw\n" % work)
os.chmod(fake, os.stat(fake).st_mode | stat.S_IEXEC)
os.environ["PATH"] = work + ":" + os.environ["PATH"]

loader = importlib.machinery.SourceFileLoader("mangos_speak", "/usr/bin/mangos-speak")
speak = importlib.util.module_from_spec(importlib.util.spec_from_loader("mangos_speak", loader))
loader.exec_module(speak)
with open("%s/%s.onnx.json" % (speak.VOICES_DIR, speak.DEFAULT_VOICE)) as f:
    sample_rate = json.load(f)["audio"]["sample_rate"]
speak.SOCKET = os.path.join(work, "sock")
speak.SETTINGS = os.path.join(work, "voice")                 # no file: the local default voice
threading.Thread(target=speak.serve, daemon=True).start()
deadline = time.time() + 60
while not os.path.exists(speak.SOCKET):
    if time.time() > deadline:
        raise SystemExit("the test service did not start within a minute")
    time.sleep(0.2)
sound = os.path.join(work, "sound.raw")


def played_bytes():
    return os.path.getsize(sound) if os.path.exists(sound) else 0


def connect():
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(speak.SOCKET)
    return sock, sock.makefile("rw", encoding="utf-8")


def send(f, message):
    f.write(json.dumps(message) + "\n")
    f.flush()


def result(f):
    """The service's result code, after the lines that report each sentence as it is heard."""
    while (line := f.readline().strip()).startswith("{"):
        pass
    return line


sock, f = connect()
start = time.time()
send(f, {"stream": True})
first = None
for phrase in ("Sì, ti sento bene.", "Adesso guardo che cosa c'è davanti al telefono.", "Ho finito, dimmi pure."):
    send(f, {"text": phrase})
    until = time.time() + 1
    while time.time() < until:
        if first is None and played_bytes():
            first = time.time() - start
        time.sleep(0.02)
send(f, {"end": True})
done = result(f)
print("first sound after %.1f s; done after %.1f s; result %s; %.1f s of audio" % (
    first if first is not None else -1, time.time() - start, done, played_bytes() / 2 / sample_rate))

# A stream left waiting (the model is still writing) must stop on the stop command.
sock, f = connect()
send(f, {"stream": True})
send(f, {"text": "Una frase sola."})
time.sleep(2.5)
start = time.time()
sock2, f2 = connect()
send(f2, {"stop": True})
print("stop on command: reply %s after %.1f s; the stream replies %s" % (result(f2), time.time() - start, result(f)))
shutil.rmtree(work)
