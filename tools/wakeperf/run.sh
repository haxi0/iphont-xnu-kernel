#!/bin/bash
# Superseded by tools/t (levels quick / power / display / gfx / full): `tools/t power` is this test, event-driven, in about
# two minutes instead of nine, with a verdict. This stays so that the old name does the right thing.
exec "$(dirname "$(readlink -f "$0")")/../t" "${1:-power}"
