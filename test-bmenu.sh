#!/usr/bin/env bash
# Pipe the Greek alphabet into bmenu as a quick manual POC test.
# Verifies the dmenu-like flow:
# - list display from stdin
# - fuzzy filtering while typing
# - enter returns the selected value on stdout.

printf '%s\n' \
    alpha beta gamma delta epsilon zeta eta theta \
    iota kappa lambda mu nu xi omicron pi \
    rho sigma tau upsilon phi chi psi omega \
    | ./build/bmenu
