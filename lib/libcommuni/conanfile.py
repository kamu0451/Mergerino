from conans import ConanFile, CMake, tools


class LibCommuniConan(ConanFile):
    name = "communi"
    version = "3.6.0"
    license = "MIT"
    author = "Edgar Edgar@AnotherFoxGuy.com"
    url = "https://github.com/AnotherFoxGuy/libcommuni-cmake"
    description = "A cross-platform IRC framework written with Qt"
    settings = "os", "compiler", "build_type", "arch"
    generators = "cmake"
    exports_sources = "include*", "src*", "CMakeLists.txt"

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.includedirs = ['include',
                                     'include/IrcUtil',
                                     'include/IrcModel',
                                     'include/IrcCore'
                                     ]
        self.cpp_info.libs = tools.collect_libs(self)
