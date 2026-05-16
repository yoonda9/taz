from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.scm import Git


class MongooseRecipe(ConanFile):
    name = "mongoose"
    version = "7.21"
    description = "Embedded networking library (HTTP/WebSocket/MQTT) for C/C++"
    license = "GPL-2.0-or-later"
    homepage = "https://mongoose.ws/"
    url = "https://github.com/cesanta/mongoose"
    topics = ("http", "websocket", "mqtt", "networking", "embedded")
    package_type = "static-library"
    source_url = "https://github.com/cesanta/mongoose.git"
    exports_sources = "CMakeLists.txt"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "fPIC": [True, False],
        "io_buf_size": ["ANY"],
    }
    default_options = {
        "fPIC": True,
        "io_buf_size": 2 * 1024,
    }

    def validate(self) -> None:
        try:
            size = int(self.options.io_buf_size)
        except ValueError as err:
            raise ConanInvalidConfiguration(
                f"io_buf_size must be an integer, got {self.options.io_buf_size!r}"
            ) from err
        if size <= 0:
            raise ConanInvalidConfiguration(
                f"io_buf_size must be a positive integer, got {size}"
            )

    def config_options(self) -> None:
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self) -> None:
        cmake_layout(self)

    def source(self) -> None:
        git = Git(self)
        clone_args = ["--depth", "1", "--branch", self.version]
        git.clone(self.source_url, args=clone_args)

    def generate(self) -> None:
        tc = CMakeToolchain(self)
        tc.preprocessor_definitions["MG_IO_SIZE"] = str(self.options.io_buf_size)
        if self.options.get_safe("fPIC"):
            tc.cache_variables["CMAKE_POSITION_INDEPENDENT_CODE"] = True
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self) -> None:
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self) -> None:
        cmake = CMake(self)
        cmake.install()

    def package_info(self) -> None:
        self.cpp_info.libs = ["mongoose"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.append("rt")
