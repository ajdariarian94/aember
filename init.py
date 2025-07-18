#!/usr/bin/env python3

import argparse

from python.base import (
    build_docker,
    run_docker
)


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "--no_tty",
        action="store_true",
        help="Run Docker container in non interactive mode",
    )

    parser.add_argument(
        "--no_gpu",
        action="store_true",
        help="Run Docker container in non interactive mode",
    )

    parser.add_argument(
        "--env",
        metavar="key=value",
        action="append",
        help="Add NAME=VALUE environment variables",
    )

    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser("build_docker")
    subparsers.add_parser("run")


    [args, unknown] = parser.parse_known_args()

    if args.env is None:
        args.env = []

    if args.command == "build_docker":
        build_docker()
    elif args.command == "run":
        run_docker(" ".join(unknown), args.no_tty, args.no_gpu, args.env)


if __name__ == "__main__":
    main()