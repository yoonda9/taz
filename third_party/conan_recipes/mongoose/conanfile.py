from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.scm import Git


class MongooseRecipe(ConanFile):
    name = "mongoose"
    version = "7.20"
    source_url = "https://github.com/cesanta/mongoose.git"
    exports_sources = "CMakeLists.txt"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "fPIC": [True, False],
        "io_buf_size_inc": ["ANY"],
    }
    default_options = {
        "fPIC": True,
        "io_buf_size_inc": 2 * 1024,
    }

    def validate(self) -> None:
        try:
            int(self.options.io_buf_size_inc)
        except ValueError as err:
            raise ConanInvalidConfiguration(err) from err

    def config_options(self) -> None:
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def source(self) -> None:
        git = Git(self)
        clone_args = ["--depth", "1", "--branch", self.version]
        git.clone(self.source_url, args=clone_args)

    def generate(self) -> None:
        tc = CMakeToolchain(self)
        tc.preprocessor_definitions["MG_IO_SIZE"] = self.options.io_buf_size_inc
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
        if self.settings.os is "Linux":
            self.cpp_info.system_libs.append("rt")
