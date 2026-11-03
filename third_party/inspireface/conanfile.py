from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, unzip, check_sha256
import os


class InspireFacePackage(ConanFile):
    name = "inspireface"
    version = "1.2.3"
    package_type = "shared-library"
    settings = "os", "arch"
    exports_sources = "sdk.zip", "Pikachu"

    def validate(self):
        if str(self.settings.os) != "Macos" or str(self.settings.arch) != "armv8":
            raise ConanInvalidConfiguration("This recipe packages the official macOS arm64 SDK")

    def build(self):
        check_sha256(self, os.path.join(self.source_folder, "sdk.zip"),
                     "96effaacc76e410b5dcec96d30abb136aea7a4126ea1e988a40e1a1b88ecde3a")
        check_sha256(self, os.path.join(self.source_folder, "Pikachu"),
                     "5037ba1f49905b783a1c973d5d58b834a645922cc2814c8e3ca630a38dc24431")
        unzip(self, os.path.join(self.source_folder, "sdk.zip"), destination=self.build_folder)

    def package(self):
        sdk = os.path.join(self.build_folder, "inspireface-macos-apple-silicon-arm64-1.2.3", "InspireFace")
        copy(self, "*", src=os.path.join(sdk, "include"), dst=os.path.join(self.package_folder, "include"))
        copy(self, "*.dylib", src=os.path.join(sdk, "lib"), dst=os.path.join(self.package_folder, "lib"))
        copy(self, "Pikachu", src=self.source_folder, dst=os.path.join(self.package_folder, "res"))

    def package_info(self):
        self.cpp_info.libs = ["InspireFace"]
        self.cpp_info.resdirs = ["res"]
        self.cpp_info.set_property("cmake_target_name", "inspireface::inspireface")
