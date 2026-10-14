#!/usr/bin/env python3
"""Can two headless Claude Code sessions talk to each other? Starts a Sonnet worker and a Haiku
lead, each with a session name (claude -p, stream-json, inbound messages accepted), asks the
lead to send the worker a question with SendMessage, and prints what each session's output shows,
with times.

Usage, on the phone, as pim, with the subscription token in CLAUDE_CODE_OAUTH_TOKEN (from
/etc/mangos/claude-token) and outside /home/pim, whose sessions PIM resumes:
  python3 -u claude_sessions.py [seconds to watch, default 150]
The sessions work in /var/tmp/claude-test/worker and /var/tmp/claude-test/lead.
"""
import json
import os
import subprocess
import sys
import threading
import time

BASE = "/var/tmp/claude-test"
SETTINGS = f"{BASE}/inbound.json"
T0 = time.time()


def start(name, model, cwd):
    os.makedirs(cwd, exist_ok=True)
    p = subprocess.Popen(
        ["claude", "-p", "--input-format", "stream-json", "--output-format", "stream-json",
         "--verbose", "--model", model, "--name", name, "--settings", SETTINGS,
         "--dangerously-skip-permissions"],
        cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1)
    threading.Thread(target=watch, args=(name, p), daemon=True).start()
    return p


def show(name, text):
    print(f"{time.time() - T0:6.1f}s [{name}] {text[:220]}", flush=True)


def watch(name, p):
    for line in p.stdout:
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            show(name, "raw: " + line.strip())
            continue
        t = e.get("type")
        if t == "assistant":
            for b in e["message"].get("content", []):
                if b.get("type") == "text" and b["text"].strip():
                    show(name, "says: " + b["text"].strip().replace("\n", " "))
                elif b.get("type") == "tool_use":
                    show(name, f"tool {b['name']}: {json.dumps(b.get('input'), ensure_ascii=False)}")
        elif t == "user":
            for b in e["message"].get("content", []) if isinstance(e["message"].get("content"), list) else []:
                if b.get("type") == "tool_result":
                    c = b.get("content")
                    show(name, "tool result: " + (json.dumps(c, ensure_ascii=False) if not isinstance(c, str) else c).replace("\n", " "))
                elif b.get("type") == "text":
                    show(name, "input text: " + b["text"].replace("\n", " "))
            if isinstance(e["message"].get("content"), str):
                show(name, "input: " + e["message"]["content"].replace("\n", " "))
        elif t == "result":
            show(name, f"result ({e.get('subtype')}): {str(e.get('result'))[:150]}")
        elif t == "system":
            show(name, f"system {e.get('subtype')} {e.get('status', '')}")
        else:
            show(name, f"event {t}")


def send(p, text):
    p.stdin.write(json.dumps({"type": "user", "message": {"role": "user", "content": text}}) + "\n")
    p.stdin.flush()


with open(SETTINGS, "w") as f:
    json.dump({"crossSessionInbound": "accept"}, f)

worker = start("worker", "sonnet", f"{BASE}/worker")
send(worker, "Sei il lavoratore di prova. Quando un'altra sessione ti scrive, rispondile con "
             "SendMessage. Per ora rispondi solo: pronto.")
time.sleep(25)
lead = start("lead", "haiku", f"{BASE}/lead")
send(lead, "Usa ListAgents per trovare l'altra sessione Claude su questo telefono (quella chiamata "
           "worker), poi mandale con SendMessage la domanda: 'Quanto fa 17 per 3? "
           "Rispondimi con SendMessage.' Poi dimmi cosa hai fatto. Non scrivere mai alla sessione "
           "chiamata pim: è un'altra persona al lavoro.")
time.sleep(int(sys.argv[1]) if len(sys.argv) > 1 else 150)
for p in (lead, worker):
    p.stdin.close()
    p.terminate()
show("test", "end")
