#!/bin/bash

set -xe

cc -o ie-debug-build *.c -Iinclude/ -O0 -ggdb -lforge
