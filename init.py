#!/usr/bin/env python3

import argparse

from python.base import (
    build_docker,
    run_docker
)


def main():
    parser = argparse.ArgumentParser()

    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser("build_docker")
    subparsers.add_parser("run")

    [args, _] = parser.parse_known_args()

    if args.command == "build_docker":
        build_docker()
    elif args.command == "run":
        run_docker()


if __name__ == "__main__":
    main()