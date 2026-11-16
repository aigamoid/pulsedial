#!/usr/bin/env python3
# PulseDial: log in to Spotify once on your PC and hand the key (refresh token) to the StopWatch.
# Uses only the Python standard library (Windows / macOS / Linux, Python 3.9+).
#
#   python3 tools/spotify_login.py                      (asks for Client ID, IP and PIN)
#   python3 tools/spotify_login.py --client-id <ID> --device <StopWatch IP> --pin <4 digits>
#
# - Open PulseDial on the StopWatch: the setup screen shows the IP and the PIN.
# - Your browser opens Spotify's login / permission page. After you allow it, the key is sent to the StopWatch.
# - The key is never printed or saved on the PC. It is stored only on the StopWatch (NVS).
# - In your Spotify developer app, add this Redirect URI:  http://127.0.0.1:8888/callback
# Scopes: read what is playing, control playback, read / change your Liked Songs.
import argparse
import base64
import hashlib
import http.server
import json
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-playback-state user-modify-playback-state user-read-currently-playing " \
         "user-library-read user-library-modify"

ap = argparse.ArgumentParser(description="Log in to Spotify and send the key to PulseDial on the StopWatch.")
ap.add_argument("--client-id", help="Client ID of your Spotify developer app")
ap.add_argument("--device", help="IP address shown on the StopWatch (e.g. 192.168.1.23)")
ap.add_argument("--pin", help="4-digit PIN shown on the StopWatch")
args = ap.parse_args()


def ask(value, prompt):
    while not value:
        value = input(prompt).strip()
    return value


client_id = ask(args.client_id, "Client ID (from developer.spotify.com/dashboard): ")
device = ask(args.device, "StopWatch IP (shown on its screen): ").removeprefix("IP ").strip()
pin = ask(args.pin, "PIN (4 digits on its screen): ").removeprefix("PIN ").strip()

verifier = secrets.token_urlsafe(64)[:96]
challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
state = secrets.token_urlsafe(16)
result = {}
done = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path != "/callback":
            self.send_response(404)
            self.end_headers()
            return
        q = urllib.parse.parse_qs(u.query)
        ok = q.get("state", [""])[0] == state and "code" in q
        result["code"] = q.get("code", [""])[0] if ok else ""
        result["error"] = q.get("error", [""])[0]
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        msg = "Got it. Sending the key to your StopWatch &mdash; you can close this tab." if ok else \
            "Spotify did not give permission. Check the terminal."
        self.wfile.write(f"<html><body style='font-family:sans-serif;padding:2em'>{msg}</body></html>".encode())
        done.set()

    def log_message(self, *a):
        pass


try:
    server = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
except OSError as e:
    sys.exit(f"Port 8888 is busy ({e}). Close the other program using it and try again.")
threading.Thread(target=server.serve_forever, daemon=True).start()
url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
    "client_id": client_id, "response_type": "code", "redirect_uri": REDIRECT, "scope": SCOPES,
    "code_challenge_method": "S256", "code_challenge": challenge, "state": state})
print("Opening Spotify in your browser...")
print("(If it does not open, copy this URL into your browser)\n" + url + "\n")
webbrowser.open(url)
if not done.wait(300):
    sys.exit("No answer from the browser in 5 minutes. Run this again.")
server.shutdown()
if not result.get("code"):
    sys.exit(f"Not allowed ({result.get('error') or 'state mismatch'}).")

# permission code -> key (refresh token)
body = urllib.parse.urlencode({"grant_type": "authorization_code", "code": result["code"], "redirect_uri": REDIRECT,
                               "client_id": client_id, "code_verifier": verifier}).encode()
req = urllib.request.Request("https://accounts.spotify.com/api/token", data=body,
                             headers={"Content-Type": "application/x-www-form-urlencoded"})
try:
    with urllib.request.urlopen(req, timeout=15) as r:
        tok = json.load(r)
except urllib.error.HTTPError as e:
    sys.exit(f"Could not get the key (HTTP {e.code}: {e.read().decode(errors='replace')[:200]}).\n"
             "Check the Client ID and that the Redirect URI is exactly " + REDIRECT)
except OSError as e:
    sys.exit(f"Could not reach Spotify ({e}).")
refresh = tok.get("refresh_token", "")
if not refresh:
    sys.exit("Spotify did not return a refresh token.")
print("Got the key from Spotify (not shown).")

# -> StopWatch
body = urllib.parse.urlencode({"pin": pin, "client_id": client_id, "refresh_token": refresh}).encode()
req = urllib.request.Request(f"http://{device}:8080/setup", data=body,
                             headers={"Content-Type": "application/x-www-form-urlencoded"})
try:
    with urllib.request.urlopen(req, timeout=10) as r:
        print("StopWatch:", r.read().decode().strip())
except urllib.error.HTTPError as e:
    sys.exit(f"The StopWatch refused it (HTTP {e.code}: {e.read().decode(errors='replace').strip()}). Check the PIN.")
except OSError as e:
    sys.exit(f"Could not reach the StopWatch ({e}). Check the IP, and that the PC and the StopWatch are on the same Wi-Fi.")
print("Done. The StopWatch screen will switch to the player.")
