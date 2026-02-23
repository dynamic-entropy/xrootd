#!/usr/bin/env bash

PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"
TESTDIR="${SOURCE_DIR}/../../python/tests"

function setup_pybind() {
	require_commands "${PYTHON}"

	"${PYTHON}" -c "import pytest" 2>/dev/null || \
		"${PYTHON}" -m pip install pytest xattr 2>/dev/null || \
		"${PYTHON}" -m pip install --break-system-packages pytest
}

function test_pybind() {
	export XROOTD_URL="${HOST}"

	"${PYTHON}"  -m pytest \
	 "${TESTDIR}/test_copy.py" \
	 "${TESTDIR}/test_file.py" \
	 "${TESTDIR}/test_filesystem.py" \
	 "${TESTDIR}/test_threads.py" \
	 "${TESTDIR}/test_glob.py" \
	 "${TESTDIR}/test_url.py" \
	 "${TESTDIR}/test_adler32_binding.py" \
	 -v

}
