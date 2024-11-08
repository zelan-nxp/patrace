#!/bin/bash

### Build integration testing binaries
#

set -e

### Sanity checking the test
#

if [ "$(pwd|sed 's/.*scripts.*/scripts/')" == "scripts" ]; then
	echo "Must be run from source root directory"
	exit 1
fi

echo "*** Testing BUILDING"
NO_PYTHON_BUILD=y scripts/build.py patrace x11_x64 debug
NO_PYTHON_BUILD=y scripts/build.py patrace x11_x64 release
NO_PYTHON_BUILD=y scripts/build.py patrace x11_x64 sanitizer
NO_PYTHON_BUILD=y scripts/build.py patrace fbdev_aarch64 release
NO_PYTHON_BUILD=y scripts/build.py patrace x11_x32 release
