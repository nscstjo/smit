"""SMIT iCast USB-C/DTMB USB Tuner HTTP control transport."""
import json
import urllib.error
import urllib.request

def call(url, path, data=None):
    if not path.startswith('/') or '?' in path or '#' in path:
        raise ValueError('API path must start with / and have no query or fragment')
    raw = None if data is None else json.dumps(data, separators=(',', ':')).encode('ascii')
    req = urllib.request.Request(url.rstrip('/') + path, data=raw,
                                 headers={'Content-Type': 'application/json'} if raw else {})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, json.load(resp)
    except urllib.error.HTTPError as exc:
        return exc.code, json.load(exc)
