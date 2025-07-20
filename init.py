#!/usr/bin/env python3

import argparse

from python.base import (
    build_docker,
    run_docker
)

def main():
    parser = argparse.ArgumentParser(description="Docker control script for aember")

    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("build_docker", help="Build the Docker container")
    subparsers.add_parser("run", help="Run the Docker container")

    [args, _] = parser.parse_known_args()

    if args.command == "build_docker":
        build_docker()
    elif args.command == "run":
        run_docker()


if __name__ == "__main__":
    main()