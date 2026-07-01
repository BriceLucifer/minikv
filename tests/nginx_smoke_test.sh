#!/usr/bin/env bash
set -euo pipefail

MKV_BIN=${1:?mkv binary path required}

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

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/minikv-nginx-smoke.XXXXXX")
VOLUME1_ROOT="$ROOT/volume1"
VOLUME2_ROOT="$ROOT/volume2"
NGINX_PREFIX="$ROOT/nginx-prefix"
DB_PATH="$ROOT/indexdb"
CONF="$ROOT/nginx.conf"
MASTER_LOG="$ROOT/master.log"
VOLUME_LOG="$ROOT/nginx.log"
PUT_BODY="$ROOT/put-body"
GET_BODY="$ROOT/get-body"
MULTIPART_BODY="$ROOT/multipart-body"
HEADERS="$ROOT/headers"

mkdir -p "$VOLUME1_ROOT" "$VOLUME2_ROOT" "$NGINX_PREFIX"

VOLUME1_PORT=$(free_port)
VOLUME2_PORT=$(free_port "$VOLUME1_PORT")
MASTER_PORT=$(free_port "$VOLUME1_PORT" "$VOLUME2_PORT")

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
error_log $VOLUME_LOG info;

events {
  worker_connections 128;
}

http {
  default_type application/octet-stream;
  access_log off;
  sendfile on;

  server {
    listen 127.0.0.1:$VOLUME1_PORT;

    location / {
      root $VOLUME1_ROOT;
      client_body_temp_path $ROOT/body_temp1;
      client_max_body_size 0;
      dav_methods PUT DELETE;
      dav_access group:rw all:r;
      create_full_put_path on;
      autoindex on;
      autoindex_format json;
    }
  }

  server {
    listen 127.0.0.1:$VOLUME2_PORT;

    location / {
      root $VOLUME2_ROOT;
      client_body_temp_path $ROOT/body_temp2;
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
wait_http_status "http://127.0.0.1:$VOLUME1_PORT/" "200"
wait_http_status "http://127.0.0.1:$VOLUME2_PORT/" "200"

"$MKV_BIN" \
  -volumes "127.0.0.1:$VOLUME1_PORT" \
  -db "$DB_PATH" \
  -replicas 1 \
  -subvolumes 1 \
  -port "$MASTER_PORT" \
  server >"$MASTER_LOG" 2>&1 &
MASTER_PID=$!
wait_http_status "http://127.0.0.1:$MASTER_PORT/__missing__" "404"

printf '%s' "hello from nginx smoke" >"$PUT_BODY"

put_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  -X PUT --data-binary @"$PUT_BODY" \
  "http://127.0.0.1:$MASTER_PORT/hello")
if [[ "$put_status" != "201" ]]; then
  echo "PUT returned $put_status" >&2
  exit 1
fi

curl -sS -D "$HEADERS" -o /dev/null \
  "http://127.0.0.1:$MASTER_PORT/hello"
if ! grep -qi '^HTTP/.* 302' "$HEADERS"; then
  echo "GET without redirect did not return 302" >&2
  cat "$HEADERS" >&2
  exit 1
fi
if ! grep -qi '^Location: http://127\.0\.0\.1:' "$HEADERS"; then
  echo "GET response did not contain volume Location" >&2
  cat "$HEADERS" >&2
  exit 1
fi
ORIGINAL_LOCATION=$(awk 'BEGIN{IGNORECASE=1} /^Location:/ {gsub("\r", "", $2); print $2; exit}' "$HEADERS")

curl -sS -L -o "$GET_BODY" "http://127.0.0.1:$MASTER_PORT/hello"
if ! cmp -s "$PUT_BODY" "$GET_BODY"; then
  echo "GET -L body did not match PUT body" >&2
  exit 1
fi

head_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  -I "http://127.0.0.1:$MASTER_PORT/hello")
if [[ "$head_status" != "302" ]]; then
  echo "HEAD returned $head_status" >&2
  exit 1
fi

s3_put_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  -X PUT --data-binary @"$PUT_BODY" \
  "http://127.0.0.1:$MASTER_PORT/bucket/hello")
if [[ "$s3_put_status" != "201" ]]; then
  echo "S3-style PUT returned $s3_put_status" >&2
  exit 1
fi

s3_list_body=$(curl -sS "http://127.0.0.1:$MASTER_PORT/bucket?list-type=2")
if [[ "$s3_list_body" != *"<ListBucketResult>"* ||
      "$s3_list_body" != *"<Contents><Key>hello</Key>"* ||
      "$s3_list_body" != *"<Size>22</Size>"* ||
      "$s3_list_body" != *"<ETag>"* ]]; then
  echo "S3 list-type=2 response did not include bucket object" >&2
  echo "$s3_list_body" >&2
  exit 1
fi

s3_delete_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  -X POST -H 'Content-Type: application/xml' \
  --data-binary '<Delete><Object><Key>hello</Key></Object></Delete>' \
  "http://127.0.0.1:$MASTER_PORT/bucket?delete")
if [[ "$s3_delete_status" != "204" ]]; then
  echo "S3 bulk delete returned $s3_delete_status" >&2
  exit 1
fi

s3_deleted_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  "http://127.0.0.1:$MASTER_PORT/bucket/hello")
if [[ "$s3_deleted_status" != "404" ]]; then
  echo "GET after S3 bulk delete returned $s3_deleted_status" >&2
  exit 1
fi

multipart_init_body=$(curl -sS -X POST \
  "http://127.0.0.1:$MASTER_PORT/bucket/multipart?uploads")
multipart_upload_id=$(printf '%s' "$multipart_init_body" |
  sed -n 's:.*<UploadId>\([^<]*\)</UploadId>.*:\1:p')
if [[ -z "$multipart_upload_id" ]]; then
  echo "multipart init did not return an UploadId" >&2
  echo "$multipart_init_body" >&2
  exit 1
fi

multipart_part1_status=$(printf '%s' "hello " | curl -sS -o /dev/null \
  -w "%{http_code}" -X PUT --data-binary @- \
  "http://127.0.0.1:$MASTER_PORT/bucket/multipart?partNumber=1&uploadId=$multipart_upload_id")
if [[ "$multipart_part1_status" != "200" ]]; then
  echo "multipart part 1 returned $multipart_part1_status" >&2
  exit 1
fi

multipart_part2_status=$(printf '%s' "multipart smoke" | curl -sS -o /dev/null \
  -w "%{http_code}" -X PUT --data-binary @- \
  "http://127.0.0.1:$MASTER_PORT/bucket/multipart?partNumber=2&uploadId=$multipart_upload_id")
if [[ "$multipart_part2_status" != "200" ]]; then
  echo "multipart part 2 returned $multipart_part2_status" >&2
  exit 1
fi

multipart_complete_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  -X POST -H 'Content-Type: application/xml' \
  --data-binary '<CompleteMultipartUpload><Part><PartNumber>1</PartNumber></Part><Part><PartNumber>2</PartNumber></Part></CompleteMultipartUpload>' \
  "http://127.0.0.1:$MASTER_PORT/bucket/multipart?uploadId=$multipart_upload_id")
if [[ "$multipart_complete_status" != "201" ]]; then
  echo "multipart complete returned $multipart_complete_status" >&2
  exit 1
fi

printf '%s' "hello multipart smoke" >"$MULTIPART_BODY"
curl -sS -L -o "$GET_BODY" \
  "http://127.0.0.1:$MASTER_PORT/bucket/multipart"
if ! cmp -s "$MULTIPART_BODY" "$GET_BODY"; then
  echo "GET -L body after multipart upload did not match completed body" >&2
  exit 1
fi

kill "$MASTER_PID" >/dev/null 2>&1 || true
wait "$MASTER_PID" >/dev/null 2>&1 || true
MASTER_PID=""
rm -rf "$DB_PATH"

"$MKV_BIN" \
  -volumes "127.0.0.1:$VOLUME1_PORT" \
  -db "$DB_PATH" \
  -replicas 1 \
  -subvolumes 1 \
  rebuild >>"$MASTER_LOG" 2>&1

"$MKV_BIN" \
  -volumes "127.0.0.1:$VOLUME1_PORT" \
  -db "$DB_PATH" \
  -replicas 1 \
  -subvolumes 1 \
  -port "$MASTER_PORT" \
  server >>"$MASTER_LOG" 2>&1 &
MASTER_PID=$!
wait_http_status "http://127.0.0.1:$MASTER_PORT/__missing__" "404"

curl -sS -L -o "$GET_BODY" "http://127.0.0.1:$MASTER_PORT/hello"
if ! cmp -s "$PUT_BODY" "$GET_BODY"; then
  echo "GET -L body after rebuild did not match PUT body" >&2
  exit 1
fi

kill "$MASTER_PID" >/dev/null 2>&1 || true
wait "$MASTER_PID" >/dev/null 2>&1 || true
MASTER_PID=""

"$MKV_BIN" \
  -volumes "127.0.0.1:$VOLUME2_PORT" \
  -db "$DB_PATH" \
  -replicas 1 \
  -subvolumes 1 \
  rebalance >>"$MASTER_LOG" 2>&1

old_volume_status=$(curl -sS -o /dev/null -w "%{http_code}" "$ORIGINAL_LOCATION")
if [[ "$old_volume_status" != "404" ]]; then
  echo "old volume returned $old_volume_status after rebalance" >&2
  exit 1
fi

"$MKV_BIN" \
  -volumes "127.0.0.1:$VOLUME2_PORT" \
  -db "$DB_PATH" \
  -replicas 1 \
  -subvolumes 1 \
  -port "$MASTER_PORT" \
  server >>"$MASTER_LOG" 2>&1 &
MASTER_PID=$!
wait_http_status "http://127.0.0.1:$MASTER_PORT/__missing__" "404"

curl -sS -D "$HEADERS" -o "$GET_BODY" -L \
  "http://127.0.0.1:$MASTER_PORT/hello"
if ! cmp -s "$PUT_BODY" "$GET_BODY"; then
  echo "GET -L body after rebalance did not match PUT body" >&2
  exit 1
fi
if ! grep -qi "Location: http://127\\.0\\.0\\.1:$VOLUME2_PORT" "$HEADERS"; then
  echo "GET after rebalance did not redirect to the new volume" >&2
  cat "$HEADERS" >&2
  exit 1
fi

delete_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  -X DELETE "http://127.0.0.1:$MASTER_PORT/hello")
if [[ "$delete_status" != "204" ]]; then
  echo "DELETE returned $delete_status" >&2
  exit 1
fi

missing_status=$(curl -sS -o /dev/null -w "%{http_code}" \
  "http://127.0.0.1:$MASTER_PORT/hello")
if [[ "$missing_status" != "404" ]]; then
  echo "GET after DELETE returned $missing_status" >&2
  exit 1
fi
