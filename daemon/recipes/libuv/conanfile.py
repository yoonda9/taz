# Derived from the conan-center-index libuv recipe, revision
# ea781dcacf58c65462ab0d3d75e9b002 of libuv/1.51.0. Kept in-tree so TAZ can
# apply its own patch series to libuv and gate it behind `legacy_fallbacks`.
# The conan-center-index licence, which covers the parts copied from it:
#
# MIT License
#
# Copyright (c) 2019 Conan.io
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
import os
from typing import ClassVar

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import (
    apply_conandata_patches,
    copy,
    export_conandata_patches,
    get,
    rmdir,
)
from conan.tools.microsoft import check_min_vs, is_msvc
from conan.tools.scm import Version

required_conan_version = ">=2"


class LibuvConan(ConanFile):
    name = "libuv"
    version = "1.51.0-taz.1"
    description = "A multi-platform support library with a focus on asynchronous I/O"
    license = "MIT"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://libuv.org"
    topics = ("asynchronous", "io", "networking", "multi-platform")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options: ClassVar[dict[str, list[bool]]] = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tests": [True, False],
        "legacy_fallbacks": [True, False],
        "force_legacy": [True, False],
    }
    default_options: ClassVar[dict[str, bool]] = {
        "shared": False,
        "fPIC": True,
        "with_tests": False,
        "legacy_fallbacks": False,
        "force_legacy": False,
    }

    def export_sources(self):
        export_conandata_patches(self)

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def layout(self):
        cmake_layout(self, src_folder="src")

    def validate(self):
        if is_msvc(self):
            check_min_vs(self, "190")
        if self.options.force_legacy and not self.options.legacy_fallbacks:
            raise ConanInvalidConfiguration(
                "force_legacy=True requires legacy_fallbacks=True"
            )

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)
        apply_conandata_patches(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["LIBUV_BUILD_TESTS"] = bool(self.options.with_tests)
        if Version(self.version) >= "1.45.0":
            tc.variables["LIBUV_BUILD_SHARED"] = self.options.shared
        if Version(self.version) < "1.47.0":
            # CMake 4 support
            tc.cache_variables["CMAKE_POLICY_VERSION_MINIMUM"] = "3.5"
        if self.options.legacy_fallbacks:
            tc.preprocessor_definitions["TAZ_LEGACY_FALLBACKS"] = "1"
        if self.options.force_legacy:
            tc.preprocessor_definitions["TAZ_FORCE_LEGACY"] = "1"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        for license_file in ["LICENSE", "LICENSE-docs"]:
            copy(
                self,
                license_file,
                src=self.source_folder,
                dst=os.path.join(self.package_folder, "licenses"),
            )
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    @property
    def _target_name(self):
        if Version(self.version) < "1.45.0":
            return "uv" if self.options.shared else "uv_a"
        return "libuv::uv" if self.options.shared else "libuv::uv_a"

    @property
    def _lib_name(self):
        if Version(self.version) < "1.45.0":
            return "uv" if self.options.shared else "uv_a"
        if is_msvc(self) and not self.options.shared:
            return "libuv"
        return "uv"

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "libuv")
        self.cpp_info.set_property("cmake_target_name", self._target_name)
        self.cpp_info.set_property(
            "pkg_config_name", "libuv" if self.options.shared else "libuv-static"
        )
        self.cpp_info.libs = [self._lib_name]
        if self.options.shared:
            self.cpp_info.defines = ["USING_UV_SHARED=1"]
        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["dl", "pthread", "rt"]
        if self.settings.os == "Windows":
            self.cpp_info.system_libs = [
                "iphlpapi",
                "psapi",
                "userenv",
                "ws2_32",
            ]
            if Version(self.version) >= "1.45.0":
                self.cpp_info.system_libs.append("dbghelp")
