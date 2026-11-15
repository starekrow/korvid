#!/bin/sh
# The simplest cook: copy_assets ROOT DEST copies one module's assets/ into its place in
# a program's output tree. kormake.lua: cook "copy_assets {in} {out}". Nothing is
# converted and nothing is removed; goldberg (the default cook) does both.
set -e
mkdir -p "$2"
cp -R "$1/." "$2/"
