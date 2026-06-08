import os
import requests
from dotenv import load_dotenv
from flask import Flask, render_template, request, redirect, url_for, session, jsonify

load_dotenv()

app = Flask(__name__)
app.secret_key = os.getenv("APP_SECRET_KEY", "change-me")

ADMIN_USER = os.getenv("APP_ADMIN_USER")
ADMIN_PASS = os.getenv("APP_ADMIN_PASS")
ESP32_BASE_URL = os.getenv("ESP32_BASE_URL")
ESP32_API_KEY = os.getenv("ESP32_API_KEY")


def config_errors():
    cfg = {
        "APP_ADMIN_USER": ADMIN_USER,
        "APP_ADMIN_PASS": ADMIN_PASS,
        "ESP32_BASE_URL": ESP32_BASE_URL,
        "ESP32_API_KEY": ESP32_API_KEY,
    }
    return [key for key, value in cfg.items() if not value]


@app.before_request
def require_login():
    if request.endpoint in ("login", "static"):
        return None
    if not session.get("authed"):
        return redirect(url_for("login"))
    return None


def esp32_request(method, path, json_payload=None, params=None, timeout=6):
    missing = config_errors()
    if missing:
        return {"ok": False, "error": "config_missing", "missing": missing}, 500

    url = ESP32_BASE_URL.rstrip("/") + path
    headers = {"X-API-KEY": ESP32_API_KEY}

    try:
        resp = requests.request(method, url, json=json_payload, params=params, headers=headers, timeout=timeout)
    except requests.RequestException as exc:
        return {"ok": False, "error": "esp32_unreachable", "detail": str(exc)}, 502

    try:
        data = resp.json()
    except ValueError:
        data = {"ok": False, "error": "bad_response", "raw": resp.text}

    return data, resp.status_code


@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        if username == ADMIN_USER and password == ADMIN_PASS:
            session["authed"] = True
            return redirect(url_for("index"))
        error = "Invalid credentials"

    return render_template("login.html", error=error)


@app.route("/logout", methods=["POST"])
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/")
def index():
    errors = config_errors()
    return render_template(
        "index.html",
        config_ok=not errors,
        config_errors=errors,
    )


@app.route("/api/enroll", methods=["POST"])
def api_enroll():
    payload = request.get_json(silent=True) or {}
    first = str(payload.get("first_name", "")).strip()
    last = str(payload.get("last_name", "")).strip()

    if not first or not last:
        return jsonify({"ok": False, "error": "missing_name"}), 400

    data, status = esp32_request("POST", "/api/enroll", {"first_name": first, "last_name": last})
    return jsonify(data), status


@app.route("/api/status", methods=["GET"])
def api_status():
    data, status = esp32_request("GET", "/api/enroll/status")
    return jsonify(data), status


@app.route("/api/events", methods=["GET"])
def api_events():
    since = request.args.get("since", "0")
    data, status = esp32_request("GET", "/api/events", params={"since": since})
    return jsonify(data), status


@app.route("/api/users", methods=["GET"])
def api_users():
    data, status = esp32_request("GET", "/api/users")
    return jsonify(data), status


@app.route("/api/users", methods=["PATCH"])
def api_users_patch():
    payload = request.get_json(silent=True) or {}
    data, status = esp32_request("PATCH", "/api/users", json_payload=payload)
    return jsonify(data), status


@app.route("/api/users", methods=["DELETE"])
def api_users_delete():
    payload = request.get_json(silent=True) or {}
    data, status = esp32_request("DELETE", "/api/users", json_payload=payload, timeout=8)
    return jsonify(data), status


@app.route("/api/reset", methods=["POST"])
def api_reset():
    data, status = esp32_request("POST", "/api/reset", timeout=15)
    return jsonify(data), status


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
