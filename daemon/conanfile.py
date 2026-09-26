"""Conan consumer recipe for the TAZER daemon.

Invoked automatically by the cmake-conan dependency provider
(CMake/cmake-conan/conan_provider.cmake) during `cmake --preset ...`, so a
plain `just configure` resolves libuv and GoogleTest without a separate
`conan install` step. nanopb is a git submodule, not a Conan package.
"""

from conan import ConanFile


class TazerDaemon(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    def requirements(self) -> None:
        self.requires("libuv/1.51.0")

    def build_requirements(self) -> None:
        self.test_requires("gtest/1.17.0")
