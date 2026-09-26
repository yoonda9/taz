"""Conan consumer recipe for the TAZER daemon.

Driven by `just configure <preset>` (tools/dev.py), which runs
`conan install` with the preset's host profile chain and output folder, then
`cmake --preset <preset>`; the preset's toolchainFile points at the
conan_toolchain.cmake generated here. nanopb is a git submodule, not a Conan
package.
"""

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class TazerDaemon(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self) -> None:
        self.requires("libuv/1.51.0")

    def build_requirements(self) -> None:
        self.test_requires("gtest/1.17.0")

    def generate(self) -> None:
        toolchain = CMakeToolchain(self)
        # daemon/CMakePresets.json is the single source of build configuration;
        # do not let Conan write a CMakeUserPresets.json next to it.
        toolchain.user_presets_path = None
        toolchain.generate()
        CMakeDeps(self).generate()
