#!/usr/bin/env python
import argparse
import os
import sys
import subprocess

from build_all import (
    build_project,
    build_android,
    all_platforms,
    linux_platforms,
)

script_dir = os.path.dirname(os.path.realpath(__file__))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Build patrace')
    parser.add_argument('project', default='patrace', choices=['patrace'], help='the only valid value is \'patrace\'')
    parser.add_argument('platform', default='x11_x64', choices=all_platforms, help="window system and architecture separated with an underscore, defined in patrace/project/cmake/toolchains")
    parser.add_argument('type', default='debug', choices=['debug', 'release', 'sanitizer', 'RelWithDebInfo'], help='the build type')
    parser.add_argument('--build-dir', default='', help='path to build directory. Defaults to \'builds\' in the top level project directory.')
    parser.add_argument('--install-dir', default='', help='path to install directory. When relative, it is relative to the build directory. Defaults to \'install\' in the top level project directory.')
    parser.add_argument('--ffi7', type=bool, default=False, help='build wayland_aarch64 with libffi.7.so. Default to build with libffi.6.so')
    parser.add_argument('--ffi8', type=bool, default=False, help='build wayland_aarch64 with libffi.8.so. Default to build with libffi.6.so')
    parser.add_argument('--static', type=bool, default=False, help='build patrace statically. Defaults to \'false\'')
    parser.add_argument('--perfperapi', type=bool, default=False, help='build patrace with per gles api perf instrumentation. Defaults to \'false\'')
    args = parser.parse_args()

    exclude = ['android', 'fbdev_x32', 'fbdev_x64', 'rhe6_x32', 'rhe6_x64']
    if not args.build_dir:
        args.build_dir = os.path.abspath(os.path.join(
            script_dir, '..', 'builds', args.project, args.platform, args.type
        ))
        if args.static and args.platform not in exclude:
            args.build_dir += '_static'
        elif args.ffi7 and args.platform in ['wayland_aarch64', 'wayland_arm_hardfloat']:
                args.build_dir += '_ffi7'
        else:
            if args.ffi8 and args.platform in ['wayland_aarch64', 'wayland_arm_hardfloat']:
                args.build_dir += '_ffi8'

    if not args.install_dir:
        args.install_dir = os.path.abspath(os.path.join(
            script_dir, '..', 'install', args.project, args.platform, args.type
        ))
        if args.static and args.platform not in exclude:
            args.install_dir += '_static'
        elif args.ffi7 and args.platform in ['wayland_aarch64', 'wayland_arm_hardfloat']:
                args.install_dir += '_ffi7'
        else:
            if args.ffi8 and args.platform in ['wayland_aarch64', 'wayland_arm_hardfloat']:
                args.install_dir += '_ffi8'

    if args.platform == 'android':
        # Run CMake first to create generated files
        build_project('x11_x64', variant=args.type, build_dir=args.build_dir, install_dir=args.install_dir,
            project_path=os.path.abspath(os.path.join(script_dir, '..', 'patrace', 'project', 'cmake')),
            cmake_defines=[], stop_early=True
        )
        # Run Android build system
        returncode = build_android(
            src_path=os.path.join(script_dir, '..'),
            product_names=[args.project],
            install_base_dir=args.install_dir
        )
    else:
        if not args.platform in linux_platforms:
            print >> sys.stderr, 'Unsupported platform: %s' % args.platform
            sys.exit(-1)
        returncode = build_project(
            platform=args.platform,
            variant=args.type,
            build_dir=args.build_dir,
            install_dir=args.install_dir,
            project_path=os.path.abspath(os.path.join(script_dir, '..', 'patrace', 'project', 'cmake')),
            cmake_defines=[],
            static=args.static,
            ffi7=args.ffi7,
            ffi8=args.ffi8,
            perfperapi=args.perfperapi
        )

    sys.exit(returncode)
