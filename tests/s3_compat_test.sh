#!/usr/bin/env bash
set -euo pipefail

MKV_BIN=${1:?mkv binary path required}
PY_TEST=${2:?s3 compatibility python test path required}
REPO_ROOT=$(cd "$(dirname "$PY_TEST")/.." && pwd)
PYTHON_BIN=$("$REPO_ROOT/tests/ensure_python_test_env.sh" "$REPO_ROOT")

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1" >&2
    exit 1
  fi
}

free_port() {
  "$PYTHON_BIN" - "$@" <<'PY'
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
require_command "$PYTHON_BIN"

"$PYTHON_BIN" "$PY_TEST" --check-deps
if [[ "${MINIKV_REQUIRE_S3_COMPAT_DEPS:-}" == "1" ]]; then
  "$PYTHON_BIN" "$PY_TEST" --require-deps
fi

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/minikv-s3-compat.XXXXXX")
VOLUME_ROOT="$ROOT/volumes"
NGINX_PREFIX="$ROOT/nginx-prefix"
DB_PATH="$ROOT/indexdb"
CONF="$ROOT/nginx.conf"
MASTER_LOG="$ROOT/master.log"
NGINX_LOG="$ROOT/nginx.log"

mkdir -p "$VOLUME_ROOT" "$NGINX_PREFIX"

VOLUME_PORTS=()
USED_PORTS=()
for _ in $(seq 1 5); do
  port=$(free_port "${USED_PORTS[@]}")
  VOLUME_PORTS+=("$port")
  USED_PORTS+=("$port")
done
MASTER_PORT=$(free_port "${USED_PORTS[@]}")

cleanup() {
  local status=$?
  if [[ "$status" -ne 0 ]]; then
    echo "--- mkv master log ---" >&2
    if [[ -f "$MASTER_LOG" ]]; then
      cat "$MASTER_LOG" >&2
    else
      echo "missing $MASTER_LOG" >&2
    fi
    echo "--- nginx log ---" >&2
    if [[ -f "$NGINX_LOG" ]]; then
      cat "$NGINX_LOG" >&2
    else
      echo "missing $NGINX_LOG" >&2
    fi
  fi
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
EOF

for index in "${!VOLUME_PORTS[@]}"; do
  volume_dir="$VOLUME_ROOT/volume$((index + 1))"
  body_temp="$ROOT/body_temp$((index + 1))"
  mkdir -p "$volume_dir" "$body_temp"
  cat >>"$CONF" <<EOF
  server {
    listen 127.0.0.1:${VOLUME_PORTS[$index]};

    location / {
      root $volume_dir;
      client_body_temp_path $body_temp;
      client_max_body_size 0;
      dav_methods PUT DELETE;
      dav_access group:rw all:r;
      create_full_put_path on;
      autoindex on;
      autoindex_format json;
    }
  }
EOF
done

cat >>"$CONF" <<EOF
}
EOF

nginx -c "$CONF" -p "$NGINX_PREFIX" &
NGINX_PID=$!
for port in "${VOLUME_PORTS[@]}"; do
  wait_http_status "http://127.0.0.1:$port/" "200"
done

VOLUMES=""
for port in "${VOLUME_PORTS[@]}"; do
  if [[ -n "$VOLUMES" ]]; then
    VOLUMES+=","
  fi
  VOLUMES+="127.0.0.1:$port"
done

"$MKV_BIN" \
  -volumes "$VOLUMES" \
  -db "$DB_PATH" \
  -replicas 3 \
  -subvolumes 10 \
  -port "$MASTER_PORT" \
  server >"$MASTER_LOG" 2>&1 &
MASTER_PID=$!
wait_http_status "http://127.0.0.1:$MASTER_PORT/__missing__" "404"

if [[ "${MINIKV_REQUIRE_S3_COMPAT_DEPS:-}" == "1" ]]; then
  export MINIKV_RUN_LARGE_S3_COMPAT="${MINIKV_RUN_LARGE_S3_COMPAT:-1}"
fi

MINIKV_S3_ENDPOINT="http://127.0.0.1:$MASTER_PORT" \
  AWS_EC2_METADATA_DISABLED=true \
  "$PYTHON_BIN" "$PY_TEST"
