#!/usr/bin/env bash
set -euo pipefail

MKV_BIN=${1:?mkv binary path required}
PY_TEST=${2:?s3 compatibility python test path required}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1" >&2
    exit 1
  fi
}

free_port() {
  python3 - "$@" <<'PY'
import socket
import sys

used = {int(port) for port in sys.argv[1:]}
while True:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    if port not in used:
        print(port)
        break
PY
}

wait_http_status() {
  local url=$1
  local expected=$2

  for _ in $(seq 1 100); do
    local status
    status=$(curl -sS -o /dev/null -w "%{http_code}" "$url" || true)
    if [[ "$status" == "$expected" ]]; then
      return 0
    fi
    sleep 0.05
  done

  echo "timed out waiting for $url to return $expected" >&2
  return 1
}

require_command nginx
require_command curl
require_command python3

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/minikv-s3-compat.XXXXXX")
VOLUME_ROOT="$ROOT/volume"
NGINX_PREFIX="$ROOT/nginx-prefix"
DB_PATH="$ROOT/indexdb"
CONF="$ROOT/nginx.conf"
MASTER_LOG="$ROOT/master.log"
NGINX_LOG="$ROOT/nginx.log"

mkdir -p "$VOLUME_ROOT" "$NGINX_PREFIX"

VOLUME_PORT=$(free_port)
MASTER_PORT=$(free_port "$VOLUME_PORT")

cleanup() {
  if [[ -n "${MASTER_PID:-}" ]]; then
    kill "$MASTER_PID" >/dev/null 2>&1 || true
    wait "$MASTER_PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "${NGINX_PID:-}" ]]; then
    kill "$NGINX_PID" >/dev/null 2>&1 || true
    wait "$NGINX_PID" >/dev/null 2>&1 || true
  fi
  rm -rf "$ROOT"
}
trap cleanup EXIT

cat >"$CONF" <<EOF
daemon off;
worker_processes 1;
pid $ROOT/nginx.pid;
error_log $NGINX_LOG info;

events {
  worker_connections 128;
}

http {
  default_type application/octet-stream;
  access_log off;
  sendfile on;

  server {
    listen 127.0.0.1:$VOLUME_PORT;

    location / {
      root $VOLUME_ROOT;
      client_body_temp_path $ROOT/body_temp;
      client_max_body_size 0;
      dav_methods PUT DELETE;
      dav_access group:rw all:r;
      create_full_put_path on;
      autoindex on;
      autoindex_format json;
    }
  }
}
EOF

nginx -c "$CONF" -p "$NGINX_PREFIX" &
NGINX_PID=$!
wait_http_status "http://127.0.0.1:$VOLUME_PORT/" "200"

"$MKV_BIN" \
  -volumes "127.0.0.1:$VOLUME_PORT" \
  -db "$DB_PATH" \
  -replicas 1 \
  -subvolumes 1 \
  -port "$MASTER_PORT" \
  server >"$MASTER_LOG" 2>&1 &
MASTER_PID=$!
wait_http_status "http://127.0.0.1:$MASTER_PORT/__missing__" "404"

MINIKV_S3_ENDPOINT="http://127.0.0.1:$MASTER_PORT" \
  AWS_EC2_METADATA_DISABLED=true \
  python3 "$PY_TEST"
