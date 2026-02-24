#!/usr/bin/env bash
set -e

: ${ADLER32:=$(command -v xrdadler32)}
: ${CRC32C:=$(command -v xrdcrc32c)}
: ${XRDCP:=$(command -v xrdcp)}
: ${XRDFS:=$(command -v xrdfs)}
: ${OPENSSL:=$(command -v openssl)}
: ${CURL:=$(command -v curl)}

# Parallel arrays for compatibility with Bash < 4
host_names=(metaman man1 man2 srv1 srv2 srv3 srv4)
host_ports=(10970 10971 10972 10973 10974 10975 10976)
host_roots=()
host_https=()

# Populate derived URLs
for ((i = 0; i < ${#host_names[@]}; i++)); do
    port="${host_ports[i]}"
    root_url="root://localhost:$port"
    http_url="${root_url/root/http}"
    host_roots+=("$root_url")
    host_https+=("$http_url")
done

get_index_for_host() {
    local host="$1"
    for ((i = 0; i < ${#host_names[@]}; i++)); do
        if [[ "${host_names[i]}" == "$host" ]]; then
            echo "$i"
            return
        fi
    done
    echo "-1"
}

get_http_url_for_host() {
    local host="$1"
    local idx
    idx=$(get_index_for_host "$host")
    if [[ "$idx" -ge 0 ]]; then
        echo "${host_https[idx]}"
    else
        echo "Unknown host: $host" >&2
        exit 1
    fi
}

get_root_url_for_host() {
    local host="$1"
    local idx
    idx=$(get_index_for_host "$host")
    if [[ "$idx" -ge 0 ]]; then
        echo "${host_roots[idx]}"
    else
        echo "Unknown host: $host" >&2
        exit 1
    fi
}

# ------------------------------------------------------------------------------
# Helper Functions
# ------------------------------------------------------------------------------

check_required_commands() {
    for prog in "$@"; do
        if [[ ! -x "$prog" ]]; then
            echo >&2 "$(basename "$0"): error: '$prog': command not found"
            exit 1
        fi
    done
}

# shellcheck disable=SC2329
cleanup() {
    echo "Cleaning up..."
}
trap "cleanup; exit 1" ABRT

setup_scitokens() {
    local issuer_dir="$XRDSCITOKENS_ISSUER_DIR"
    local token_file="${PWD}/generated_tokens/token"

    if ! "$XRDSCITOKENS_CREATE_TOKEN" \
        "$issuer_dir/issuer_pub_1.pem" \
        "$issuer_dir/issuer_key_1.pem" \
        test_1 \
        "https://localhost:7095/issuer/one" \
        "storage.modify:/ storage.create:/ storage.read:/" \
        1800 >"$token_file"; then
        echo "Failed to create token"
        exit 1
    fi
    chmod 0600 "$token_file"
}

upload_file_to_host() {
    local host="$1"
    local remote_name="$2"
    local file_path="$3"

    local http_url
    http_url=$(get_http_url_for_host "$host")

    echo -e "\nUploading '$file_path' to '${http_url}/$RMTDATADIR/$remote_name'"
    if ! ${CURL} --location-trusted -v -L -s -H "Authorization: Bearer ${BEARER_TOKEN}" \
        "${http_url}/$RMTDATADIR/$remote_name" -T "$file_path"; then
        echo "Upload to $host failed!"
        exit 1
    fi
}

perform_rename() {
    local src_host="$1"
    local old_name="$2"
    local new_name="$3"
    local expected_code="$4"

    local http_url
    http_url=$(get_http_url_for_host "$src_host")

    local src_url="${http_url}/$RMTDATADIR/$old_name"
    local dst_url="${http_url}/$RMTDATADIR/$new_name"

    echo -e "\nRenaming '$src_url' to '$dst_url'"

    local tmp_body
    local tmp_stderr
    tmp_body=$(mktemp /tmp/rename_curl_body_XXXX)
    tmp_stderr=$(mktemp /tmp/rename_curl_stderr_XXXX)

    local http_code
    http_code=$(${CURL} --location-trusted -v -s -S -L -X MOVE -o "$tmp_body" -w "%{http_code}" -v \
        -H "Authorization: Bearer ${BEARER_TOKEN}" \
        -H "Destination: $dst_url" \
        "$src_url" 2>"$tmp_stderr")

    if [[ "$http_code" != "$expected_code" ]]; then
        echo "Rename failed from '$old_name' to '$new_name': expected $expected_code, got $http_code"
        echo "----- CURL VERBOSE LOG -----"
        cat "$tmp_stderr"
        echo "----- CURL RESPONSE BODY -----"
        cat "$tmp_body"
        echo "------------------------------"
        rm -f "$tmp_body" "$tmp_stderr"
        exit 1
    fi

    rm -f "$tmp_body" "$tmp_stderr"
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------

check_required_commands "$ADLER32" "$CRC32C" "$XRDCP" "$XRDFS" "$OPENSSL" "$CURL"

# The copy of cgi parameters to destination ensure that the move suceeds
src_hosts=(metaman man1 srv1)
src_codes=(501 201 201)

RMTDATADIR="/srvdata"
LCLDATADIR="${PWD}/localdata"
mkdir -p "$LCLDATADIR" "$PWD/generated_tokens"
mkdir -p "$XDG_CACHE_HOME/scitokens" && rm -rf "$XDG_CACHE_HOME/scitokens"/*

setup_scitokens
export BEARER_TOKEN_FILE="${PWD}/generated_tokens/token"
BEARER_TOKEN=$(cat "$BEARER_TOKEN_FILE")
export BEARER_TOKEN

# Create random file
testfile="${LCLDATADIR}/randomfile.ref"
${OPENSSL} rand -out "$testfile" $((1024 * (RANDOM + 1)))


for ((i = 0; i < ${#src_hosts[@]}; i++)); do
    src="${src_hosts[i]}"
    upload_file_to_host "$src" "old_$src" "$testfile"
done

for ((i = 0; i < ${#src_hosts[@]}; i++)); do
    src="${src_hosts[i]}"
    code="${src_codes[i]}"
    perform_rename "$src" "old_$src" "new_$src" "$code"
done

#
# ------------------------------------------------------------------------------
# OpenVerify plugin integration tests (cache)
# ------------------------------------------------------------------------------
#
# The OpenVerify plugin runs on the server side after cmsd selection (i.e., after
# the underlying ofs returns SFS_REDIRECT). We validate that:
# - First access causes a cache miss and triggers open_verify + cache insert.
# - Second access (with opposite time parity) uses cached result instead of
#   re-running open_verify.
#


echo -e "\n[OpenVerifyCache] Starting cache integration test \n"

assert_log_contains() {
    local logfile="$1"
    local needle="$2"
    local deadline=$((SECONDS + 10))
    while [[ $SECONDS -lt $deadline ]]; do
        if grep -F -q "$needle" "$logfile"; then
            return 0
        fi
        sleep 0.2
    done

    echo "Expected log to contain: $needle"
    echo "---- $logfile (tail) ----"
    tail -n 200 "$logfile" || true
    echo "-------------------------"
    exit 1
}

META_LOG="metaman/xrootd.log"
if [[ ! -f "$META_LOG" ]]; then
    echo "Expected metaman log at '$META_LOG' but file does not exist"
    exit 1
fi

# Ensure there's a dedicated object for the HTTP OpenVerify test.
upload_file_to_host "srv1" "openverify_http_srv1" "$testfile"

# Use a file that lives on a data server, accessed via the meta manager.
META_ROOT="root://localhost:10970"
REMOTE_PATH="/srvdata/new_srv1"
REMOTE_URL="${META_ROOT}//${REMOTE_PATH#/}"

tmp_out="${LCLDATADIR}/openverify_cache_read.out"

echo -e "\n[OpenVerifyCache] Cache integration test"
# truncate -s 0 "$META_LOG"
${XRDCP} -f "$REMOTE_URL" "$tmp_out"
assert_log_contains "$META_LOG" "openverify cache miss for"

# Extract the cache key we observed in the log; it should be the last token.
cache_key=$(grep -F "openverify cache miss for" "$META_LOG" | head -n 1 | awk '{print $NF}')
if [[ -z "$cache_key" ]]; then
    echo "Failed to extract cache key from log."
    echo "---- $META_LOG (tail) ----"
    tail -n 200 "$META_LOG" || true
    echo "--------------------------"
    exit 1
fi

expected_cached=""
if grep -F -q "openverify succeeded for $cache_key" "$META_LOG"; then
    expected_cached="openverify succeeded (cached) for $cache_key"
elif grep -F -q "openverify failed for $cache_key" "$META_LOG"; then
    expected_cached="openverify failed (cached) for $cache_key"
else
    echo "Did not find openverify result line in log after miss."
    echo "---- $META_LOG (tail) ----"
    tail -n 200 "$META_LOG" || true
    echo "--------------------------"
    exit 1
fi

${XRDCP} -f "$REMOTE_URL" "$tmp_out"
assert_log_contains "$META_LOG" "$expected_cached"

miss_count=$(grep -F -c "openverify cache miss for $cache_key" "$META_LOG" || true)
if [[ "$miss_count" -ne 1 ]]; then
    echo "Expected exactly one cache-miss log line; got $miss_count"
    echo "---- $META_LOG (tail) ----"
    tail -n 200 "$META_LOG" || true
    echo "--------------------------"
    exit 1
fi

echo -e "\n[OpenVerifyCache][HTTP] Cache integration test"
# truncate -s 0 "$META_LOG"

META_HTTP="$(get_http_url_for_host metaman)"
REMOTE_HTTP_PATH="/srvdata/openverify_http_srv1"
REMOTE_HTTP_URL="${META_HTTP}${REMOTE_HTTP_PATH}"

${CURL} -L --location-trusted -v -s -f -H "Authorization: Bearer ${BEARER_TOKEN}" "$REMOTE_HTTP_URL" -o "$tmp_out"
assert_log_contains "$META_LOG" "openverify cache miss for"

cache_key_http=$(grep -F "openverify cache miss for" "$META_LOG" | head -n 1 | awk '{print $NF}')
if [[ -z "$cache_key_http" ]]; then
    echo "Failed to extract HTTP cache key from log."
    echo "---- $META_LOG (tail) ----"
    tail -n 200 "$META_LOG" || true
    echo "--------------------------"
    exit 1
fi

expected_cached_http=""
if grep -F -q "openverify succeeded for $cache_key_http" "$META_LOG"; then
    expected_cached_http="openverify succeeded (cached) for $cache_key_http"
elif grep -F -q "openverify failed for $cache_key_http" "$META_LOG"; then
    expected_cached_http="openverify failed (cached) for $cache_key_http"
else
    echo "Did not find openverify result line in log after HTTP miss."
    echo "---- $META_LOG (tail) ----"
    tail -n 200 "$META_LOG" || true
    echo "--------------------------"
    exit 1
fi

${CURL} -L --location-trusted -v -s -f -H "Authorization: Bearer ${BEARER_TOKEN}" "$REMOTE_HTTP_URL" -o "$tmp_out"
assert_log_contains "$META_LOG" "$expected_cached_http"

miss_count_http=$(grep -F -c "openverify cache miss for $cache_key_http" "$META_LOG" || true)
if [[ "$miss_count_http" -ne 1 ]]; then
    echo "Expected exactly one HTTP cache-miss log line; got $miss_count_http"
    echo "---- $META_LOG (tail) ----"
    tail -n 200 "$META_LOG" || true
    echo "--------------------------"
    exit 1
fi

echo -e "\nALL TESTS PASSED"
exit 0
