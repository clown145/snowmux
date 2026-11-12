"""Prepare the pinned MOBI/KF8 reader and build its EPUB reconstruction features."""

from pathlib import Path

Import("env")  # noqa: F821


def patch_include(path, old, new):
    contents = path.read_bytes()
    if old in contents:
        path.write_bytes(contents.replace(old, new))
    elif new not in contents:
        raise RuntimeError(f"Unexpected include in {path}")


def prepare_sources(source):
    # This private header would shadow CrossMux Memory.h on Windows.
    original = source / "memory.h"
    renamed = source / "mobi_memory.h"
    if not original.is_file() and not renamed.is_file():
        raise RuntimeError(f"Missing pinned libmobi sources in {source}")
    for name in ("memory.c", "read.h", "index.c", "util.h"):
        patch_include(source / name, b'#include "memory.h"', b'#include "mobi_memory.h"')
    if original.is_file():
        original.rename(renamed)

    # Both libraries must use the same miniz symbols and ROM-safe configuration.
    patch_include(source / "util.h", b'#include "miniz.h"', b'#include "MinizConfig.h"')


def configure_library(build_env, node):
    if Path(str(node)).name in {"encryption.c", "miniz.c", "randombytes.c", "sha1.c", "write.c"}:
        return None
    flags = [
        "-DUSE_XMLWRITER",
        "-DUSE_MINIZ",
        "-Dmalloc=xuesuMobiMalloc",
        "-Dcalloc=xuesuMobiCalloc",
        "-Drealloc=xuesuMobiRealloc",
    ]
    miniz_headers = str(Path(build_env.subst("$PROJECT_DIR")) / "lib/miniz/src")
    return build_env.Object(node, CCFLAGS=build_env.get("CCFLAGS", []) + flags,
                            CPPPATH=build_env.get("CPPPATH", []) + [miniz_headers])[0]


if env.get("ARDUINO_LIB_COMPILE_FLAG") != "Build":
    dependencies = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    prepare_sources(dependencies / "libmobi/src")

env.AddBuildMiddleware(configure_library, "*/libmobi/src/*.c")
