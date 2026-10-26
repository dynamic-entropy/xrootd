#!/bin/bash
# Close the COPY client while a pull is in progress and check that the
# destination does not become a full copy.
#
# The server notices a dropped client at the next performance marker (5s). A
# localhost pull of a few hundred megabytes can finish before that marker, so
# the source server is frozen once bytes are flowing. The pull cannot finish
# while the source is frozen. After the client is gone, the source is thawed.
# A transfer that ignored the disconnect keeps writing; one that was cancelled
# does not.

cancel_src_disk="${PWD}/data/srv1${RMTDATADIR}/cancel_src.ref"
cancel_dst_disk="${PWD}/data/srv2${RMTDATADIR}/cancel_dst.ref"
cancel_src_url="https://localhost:10951${RMTDATADIR}/cancel_src.ref"
cancel_dst_url="https://localhost:10952${RMTDATADIR}/cancel_dst.ref"
# Sparse. Only the bytes actually pulled are written to disk.
cancel_src_size=$((2 * 1024 * 1024 * 1024))
cancel_src_pid=""
cancel_copy_pid=""
cancel_src_paused=0

file_size() {
    if [[ -f "$1" ]]; then
        stat -c %s "$1"
    else
        echo 0
    fi
}

resume_cancel_src() {
    if [[ "${cancel_src_paused}" -eq 1 ]]; then
        kill -CONT "${cancel_src_pid}" 2>/dev/null || true
        cancel_src_paused=0
    fi
}

stop_cancel_copy() {
    local pid="${cancel_copy_pid}"
    [[ -n "${pid}" ]] || return 0
    cancel_copy_pid=""
    if kill -0 "${pid}" 2>/dev/null; then
        kill -TERM "${pid}" 2>/dev/null || true
        local _
        for _ in {1..20}; do
            kill -0 "${pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${pid}" 2>/dev/null; then
            kill -KILL "${pid}" 2>/dev/null || true
        fi
    fi
    wait "${pid}" 2>/dev/null || true
}

fail_cancel() {
    stop_cancel_copy
    resume_cancel_src
    error "$*"
}

# The shared EXIT trap only cleans files. Resume a frozen source first, or the
# cleanup's xrdfs calls hang on it.
trap 'resume_cancel_src; stop_cancel_copy; cleanup' EXIT

mkdir -p "$(dirname "${cancel_src_disk}")" "$(dirname "${cancel_dst_disk}")"
rm -f "${cancel_src_disk}" "${cancel_dst_disk}"
truncate -s "${cancel_src_size}" "${cancel_src_disk}"

if [[ ! -s "${PWD}/srv1/xrootd.pid" ]]; then
    error "source server pid file is missing"
fi
cancel_src_pid="$(ps -o pid= "$(cat "${PWD}/srv1/xrootd.pid")" | tr -d '[:space:]')"
if [[ -z "${cancel_src_pid}" ]]; then
    error "source server is not running"
fi

${CURL} -X COPY -L -s -o /dev/null \
    -H "Source: ${cancel_src_url}" \
    -H "Authorization: Bearer ${BEARER_TOKEN}" \
    -H "TransferHeaderAuthorization: Bearer ${BEARER_TOKEN}" \
    --cacert "${BINARY_DIR}/tests/issuer/tlsca.pem" \
    --max-time 60 \
    "${cancel_dst_url}" &
cancel_copy_pid=$!

deadline=$((SECONDS + 20))
while (( SECONDS < deadline )); do
    if [[ "$(file_size "${cancel_dst_disk}")" -gt 0 ]]; then
        break
    fi
    sleep 0.1
done

if [[ "$(file_size "${cancel_dst_disk}")" -eq 0 ]]; then
    fail_cancel "cancelled COPY produced no data at the destination"
fi
if [[ "$(file_size "${cancel_dst_disk}")" -ge "${cancel_src_size}" ]]; then
    fail_cancel "COPY finished before the client could disconnect"
fi

if ! kill -STOP "${cancel_src_pid}"; then
    fail_cancel "could not pause the source server"
fi
cancel_src_paused=1

# Drop the COPY client. srv2 notices at its next performance marker.
stop_cancel_copy

# Marker period is 5s; the worker then aborts the paused pull.
sleep 8

size_at_cancel="$(file_size "${cancel_dst_disk}")"
resume_cancel_src

# A pull that kept running writes more once the source is thawed.
sleep 3
size_after="$(file_size "${cancel_dst_disk}")"

if [[ "${size_after}" -ge "${cancel_src_size}" ]]; then
    fail_cancel "cancelled COPY ran to completion: destination size ${size_after} matches the source"
fi
if [[ "${size_after}" -gt "${size_at_cancel}" ]]; then
    fail_cancel "cancelled COPY kept writing after the client disconnected: ${size_at_cancel} then ${size_after}"
fi
