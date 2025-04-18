#!/usr/bin/env python3
# Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
# or more contributor license agreements. Licensed under the Elastic License
# 2.0 and the following additional limitation. Functionality enabled by the
# files subject to the Elastic License 2.0 may only be used in production when
# invoked by an Elasticsearch process with a license key installed that permits
# use of machine learning features. You may not use this file except in
# compliance with the Elastic License 2.0 and the foregoing additional
# limitation.
#
# This script generates a build pipeline for Macos.
#
# OUTPUT:
#   - macos build JSON file.
#

import argparse
import json
import os

from itertools import product

archs = [
  "aarch64"
]
build_types = [
  "RelWithDebInfo",
]
actions = [
  "build",
  "debug"
]
agents = {
   "aarch64": {
      "provider": "orka",
      "image": "ml-macos-13-arm-004.orkasi"
   }
}
envs = {
    "aarch64": {
      "TMPDIR": "/tmp",
      "HOMEBREW_PREFIX": "/opt/homebrew",
      "PATH": "/opt/homebrew/bin:$PATH",
      "ML_DEBUG": "0",
      "CPP_CROSS_COMPILE": "",
      "CMAKE_FLAGS": "-DCMAKE_TOOLCHAIN_FILE=cmake/darwin-aarch64.cmake",
      "RUN_TESTS": "true",
      "BOOST_TEST_OUTPUT_FORMAT_FLAGS": "--logger=JUNIT,error,boost_test_results.junit",
    }
}

def main(args):
    pipeline_steps = []
    cur_build_types = build_types
    if args.build_type is not None:
        cur_build_types = [args.build_type]

    for arch, build_type in product(archs, cur_build_types):
        pipeline_steps.append({
            "label": f"Build & test :cpp: for MacOS-{arch}-{build_type} :macos:",
            "timeout_in_minutes": "300",
            "agents": agents[arch],
            "commands": [
              f'if [[ "{args.action}" == "debug" ]]; then export ML_DEBUG=1; fi',
              ".buildkite/scripts/steps/build_and_test.sh"
            ],
            "depends_on": "check_style",
            "key": f"build_test_macos-{arch}-{build_type}",
            "env": envs[arch],
            "artifact_paths": "*/**/unittest/boost_test_results.junit;*/**/unittest/ml_test_*",
            "plugins": {
              "test-collector#v1.2.0": {                                                              
                "files": "*/*/unittest/boost_test_results.junit",
                "format": "junit"
              }
            },
            "notify": [
              {
                "github_commit_status": {
                  "context": f"Build and test on MacOS {arch} {build_type}",
                },
              },
            ],
        })

    pipeline = {
        "steps": pipeline_steps,
    }
    print(json.dumps(pipeline, indent=2))


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument('--build-type',
                        required=False,
                        choices=build_types,
                        default=None,
                        help="Specify a specific build type to build")
    parser.add_argument('--action',
                        required=False,
                        choices=actions,
                        default="build",
                        help="Specify a build action")
    parser.add_argument("--build-aarch64",
                        required=False,
                        action='store_true',
                        default=False,
                        help="Build for aarch64?.")

    args = parser.parse_args()

    main(args)
