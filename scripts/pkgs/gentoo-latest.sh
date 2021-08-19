#!/usr/bin/env bash
echo ""
echo ""
echo "build requires linking against ncurses AND tinfo, run the following before compilation:"
echo "export LDFLAGS=\"-ltinfo -lncurses\""

#emerge --sync

# install dependencies
echo "Install the following dependencies:"
emerge $(< scripts/pkgs/gentoo-latest.txt )
