#!/usr/bin/env bash

# the xrdcp option table, which the header argument never reaches

bats_require_minimum_version 1.5.0

bats_load_library 'bats-support'
bats_load_library 'bats-assert'

load ../../helper/common.bash

export XRD_PLUGINCONFDIR="$BATS_TEST_DIRNAME"

#
# the option takes a mandatory argument
#

# Usage() exits 22. A crash is 139, and `run !` treats that as success, so
# these tests require 22. On musl, a short option that needs an argument
# and follows an operand leaves a null argv slot; `file -H` hits it.
@test "-H with no argument fails" {
	run -22 xrdcp -H
	assert_output --partial "argument missing"
}

@test "-H after an operand fails" {
	run -22 xrdcp file -H
	assert_output --partial "argument missing"
}

@test "--header with no argument fails" {
	run -22 xrdcp --header
	assert_output --partial "argument missing"
}

#
# options -H took over
#

@test "--license is still available after -H was reassigned" {
	run xrdcp --license
	assert_output --partial 'GNU Lesser General Public License'
}
