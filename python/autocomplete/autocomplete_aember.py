#!/usr/bin/env python3

import os
import sys
import glob

def autocomplete():
    options = [
        "configure",
        "build"
    ]


    args = sys.argv[1:]

    if len(args) == 1:
        prefix = sys.argv[1] if len(sys.argv) > 1 else ""

        matches = []

        for option in options:
            if option.startswith(prefix):
                matches.append(option)

        if len(matches) > 1:
            print(" ".join(matches))
        elif len(matches) == 1:
            print(matches[0])

if __name__ == "__main__":
    autocomplete()