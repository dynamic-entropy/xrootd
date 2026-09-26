#!/bin/bash
# Close the COPY client early and check that the destination is not a full copy.
# The HTTP status of a finished transfer is asserted separately; the outcome of a
# successful COPY is the last line of the chunked body (see assert_tpc_success).

src_local="${LCLDATADIR}/cancel_src.ref"
src_url="https://localhost:10951/${RMTDATADIR}/cancel_src.ref"
dst_url="https://localhost:10952/${RMTDATADIR}/cancel_dst.ref"
dst_disk="${PWD}/data/srv2/srvdata/tpc/cancel_dst.ref"

rm -f "${src_local}" "${dst_disk}"

# Large enough that the copy is still running when the client goes away.
generate_file_of_size "${src_local}" $((128 * 1024 * 1024))
upload_file "${src_local}" "${src_url}" http

# --max-time closes the client. curl's non-zero status is the disconnect.
${CURL} -X COPY -L -s -o /dev/null \
    -H "Source: ${src_url}" \
    -H "Authorization: Bearer ${BEARER_TOKEN}" \
    -H "TransferHeaderAuthorization: Bearer ${BEARER_TOKEN}" \
    --cacert "${BINARY_DIR}/tests/issuer/tlsca.pem" \
    --max-time 1 \
    "${dst_url}" || true

# The server learns the client is gone at the next performance marker (5s).
sleep 8

src_size=$(stat -c %s "${src_local}")
if [[ -f "${dst_disk}" ]]; then
    dst_size=$(stat -c %s "${dst_disk}")
else
    dst_size=0
fi

if [[ "${dst_size}" -eq "${src_size}" ]]; then
    error "cancelled COPY ran to completion: destination size ${dst_size} matches the source"
fi
