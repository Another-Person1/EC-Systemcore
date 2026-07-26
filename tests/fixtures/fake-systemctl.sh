#!/bin/sh
set -eu

: "${EC_SYSTEMCORE_TEST_SYSTEMCTL_LOG:?test log is required}"
printf '%s\n' "$*" >> "${EC_SYSTEMCORE_TEST_SYSTEMCTL_LOG}"
