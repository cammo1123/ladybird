#!/bin/python3

import argparse
import os
import subprocess
import platform
import shutil
import sys


def die(message: str) -> None:
    print(f"Error: {message}")
    exit(1)


def TODO() -> None:
    print("TODO")
    exit(1)

def is_supported_compiler(path: str) -> bool:
	if path is None:
		return False

	try:
		dump_version = run_command([path, "-dumpversion"], stderr=subprocess.STDOUT)
		version = run_command([path, "--version"], stderr=subprocess.STDOUT)
	except subprocess.CalledProcessError:
		return False

	if dump_version is None:
		return False

	major_version = int(dump_version.split(".")[0])
	if "Apple clang" in version:
		return major_version >= 14
	elif "clang" in version:
		return major_version >= 17
	else:
		return major_version >= 13

def find_newest_compiler(compilers: [str]) -> str:
	best_version = 0
	best_compiler = None

	for compiler in compilers:
		try:
			dump_version = run_command([compiler, "-dumpversion"], stderr=subprocess.STDOUT)
		except Exception:
			continue

		if dump_version is None:
			continue

		try:
			major_version = int(dump_version.split(".")[0])
		except ValueError:
			continue

		if major_version > best_version:
			best_version = major_version
			best_compiler = compiler

	return best_compiler

def pick_host_compiler() -> [str, str]:
	if is_supported_compiler(os.environ.get("CC")) and is_supported_compiler(os.environ.get("CXX")):
		return [os.environ.get("CC"), os.environ.get("CXX")]

	supported_clang = ["clang", "clang-17", "clang-18", "/opt/homebrew/opt/llvm/bin/clang"]
	host_compiler = find_newest_compiler(supported_clang)
	if is_supported_compiler(host_compiler):
		return [host_compiler, host_compiler.replace("clang", "clang++")]

	supported_gcc = ["egcc", "gcc", "gcc-13", "gcc-14", "/usr/local/bin/gcc-{13,14}", "/opt/homebrew/bin/gcc-{13,14}"]
	host_compiler = find_newest_compiler(supported_gcc)
	if is_supported_compiler(host_compiler):
		return [host_compiler, host_compiler.replace("gcc", "g++")]
	
	if platform.system() == "Darwin":
		die("Please make sure that Xcode 14.3, Homebrew Clang 17, or higher is installed.")
	else:
		die("Please make sure that GCC version 13, Clang version 17, or higher is installed.")


def run_command(command: str, stderr=None) -> str:
    return subprocess.getoutput(command).strip()

def get_top_dir() -> str:
    try:
        top_dir = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.STDOUT,
            universal_newlines=True
        ).strip()
        return top_dir
    except subprocess.CalledProcessError as e:
        print(f"Error getting top directory: {e.output.strip()}")
        return None

class Serenity:
    BUILD_DIR = None
    SUPER_BUILD_DIR = None
    LADYBIRD_SOURCE_DIR = ""
    CMAKE_ARGS = []
    CMAKE_PATH = "cmake"
    BUILD_PRESET = ""
    environment = ""

    def __init__(self, args):
        self.args = args

        self.command = args.command
        self.target = args.target
        self.environment = os.environ.copy()
        if platform.system() == "Windows":
            self.BUILD_PRESET = os.getenv('BUILD_PRESET', 'windows_ninja')
        else:
            self.BUILD_PRESET = os.getenv('BUILD_PRESET', 'default')
        
    def build_vcpkg(self):
        working_dir = os.getcwd()
        os.chdir(f"{self.LADYBIRD_SOURCE_DIR}/Toolchain")
        subprocess.run("python ./BuildVcpkg.py", env=self.environment)
        os.chdir(working_dir)

    def run(self):
        self.cmd_with_target()

        if self.command == "recreate" and self.command == "rebuild":
            self.delete_target()

        self.ensure_toolchain()
        self.ensure_target()

        match self.command:
            case "build":
                self.build_target()
            case "install":
                self.build_target()
                self.build_target("install")
            case "image":
                self.lagom_unsupported()
                self.build_target()
                self.build_target("install")
                self.build_image()
            case "copy-src":
                self.lagom_unsupported()
                self.build_target()
                self.build_target("install")
                os.environ["SERENITY_COPY_SOURCE"] = 1
                self.build_image()
            case "run":
                self.build_and_run_lagom_target("run-lagom-target")
            case "test":
                self.build_target()
                if self.target == "lagom":
                    self.run_tests()

            case _:
                self.build_target(self.command)

    def run_tests(self):
        self.environment["CTEST_OUTPUT_ON_FAILURE"] = "1"
        if (len(self.args.args) > 0):
            subprocess.run("ctest -R " + self.args.args[0], shell=True, cwd=self.BUILD_DIR, env=self.environment)
        else:
            subprocess.run("ctest", shell=True, cwd=self.BUILD_DIR, env=env)

    def build_image(self):
        if self.SERENITY_RUN == "limine":
            self.build_target("limine-image")
        else:
            self.build_target("qemu-image")

    def ensure_target(self):
        if not os.path.exists(f"{self.BUILD_DIR}/build.ninja"):
            self.create_build_dir()

    def build_and_run_lagom_target(self, run_target: str):
        lagom_target: str = self.args.target

        cmd_args = []
        for arg in self.args.args:
            cmd_args.append(arg.replace(";", "\\;"))
        lagom_args = " ".join(cmd_args)

        self.build_target(lagom_target)
        
        if (lagom_target in ["headless-browser", "ImageDecoder", "Ladybird", "RequestServer", "WebContent", "WebDriver", "WebWorker"] and platform.system() == "Darwin"):
            subprocess.run(f"{self.BUILD_DIR}/bin/Ladybird.app/Contents/MacOS/{lagom_target} " + lagom_args, env=self.environment)
        else:
            subprocess.run(f"{self.BUILD_DIR}/bin/{lagom_target}" + lagom_args, env=self.environment)

    def build_target(self, target: str = None):
        make_jobs = os.getenv("MAKEJOBS", str(os.cpu_count()))

        if target is not None:
            subprocess.run(f'ninja -j {make_jobs} -C "{self.BUILD_DIR}" -- "{target}"', env=self.environment)
        else:
            os.environ["CMAKE_BUILD_PARALLEL_LEVEL"] = make_jobs
            subprocess.run(f'{self.CMAKE_PATH} --build "{self.BUILD_DIR}"', env=self.environment)

    def create_build_dir(self):
        top_dir = subprocess.check_output(["cmake", "--version"], stderr=subprocess.STDOUT, universal_newlines=True).strip()
        top_dir = top_dir.split("\n")[0][14:]
        
        if (top_dir != "3.30.5"):
            die("Unsupported Cmake")
        
        subprocess.run(f'{self.CMAKE_PATH} --preset {self.BUILD_PRESET} {" ".join(self.CMAKE_ARGS)} -S "{self.LADYBIRD_SOURCE_DIR}" -B "{self.BUILD_DIR}"', env=self.environment)

    def ensure_toolchain(self):
        if platform.system() == "Windows":
            # Windows fails to use vcpkg f these aren't set, this is a bug
            # temporary workaround:
            # https://github.com/microsoft/vcpkg/issues/41199#issuecomment-2378255699
            self.environment["SystemDrive"] = self.environment["SYSTEMDRIVE"]
            self.environment["SystemRoot"] = self.environment["SYSTEMROOT"]
            self.environment["windir"] = self.environment["WINDIR"]
        
        self.build_vcpkg()

    def delete_target(self):
        if self.BUILD_DIR is not None and os.path.exists(self.BUILD_DIR):
            shutil.rmtree(self.BUILD_DIR)
            
        vcpkg_user_vars = f"{self.LADYBIRD_SOURCE_DIR}/Meta/CMake/vcpkg/user-variables.cmake"
        if (os.path.exists(vcpkg_user_vars)):
            os.remove(vcpkg_user_vars)

    def cmd_with_target(self):
        [CC, CXX] = pick_host_compiler()
        self.CMAKE_ARGS.append(f"-DCMAKE_C_COMPILER={CC}")
        self.CMAKE_ARGS.append(f"-DCMAKE_CXX_COMPILER={CXX}")

        self.ensure_ladybird_source_dir()

        self.BUILD_DIR = self.get_build_dir(self.BUILD_PRESET)
        self.CMAKE_ARGS.append(f"-DCMAKE_INSTALL_PREFIX={self.LADYBIRD_SOURCE_DIR}/Build/ladybird-install-{self.BUILD_PRESET}")
        
        self.environment["PATH"] = os.pathsep.join([f"{self.LADYBIRD_SOURCE_DIR}/Toolchain/Local/cmake/bin", f"{self.LADYBIRD_SOURCE_DIR}/Toolchain/Local/vcpkg/bin"]) + os.pathsep + self.environment["PATH"]
        self.environment["VCPKG_ROOT"] = f"{self.LADYBIRD_SOURCE_DIR}/Toolchain/Tarballs/vcpkg"

    def ensure_ladybird_source_dir(self):
        self.LADYBIRD_SOURCE_DIR = os.getenv('LADYBIRD_SOURCE_DIR')
        
        if not self.LADYBIRD_SOURCE_DIR or not os.path.isdir(self.LADYBIRD_SOURCE_DIR):
            self.LADYBIRD_SOURCE_DIR = get_top_dir()
        
        self.environment["LADYBIRD_SOURCE_DIR"] = self.LADYBIRD_SOURCE_DIR

    def get_build_dir(self, arg):
        self.ensure_ladybird_source_dir()
        
        if (arg == "default" or arg == "windows_ninja"):
            return f"{self.LADYBIRD_SOURCE_DIR}/Build/ladybird"
        elif (arg == "Debug"):
            return f"{self.LADYBIRD_SOURCE_DIR}/Build/ladybird-debug"
        elif (arg == "Sanitizer"):
            return f"{self.LADYBIRD_SOURCE_DIR}/Build/ladybird-sanitizers"
        else:
            die(f"Unknwon BUILD_PRESET: '{arg}'")        

   


def main():
    name = os.path.basename(__file__)
    epilog = f"""Usage: $NAME COMMAND [ARGS...]
  Supported COMMANDs:
    build:      Compiles the target binaries, [ARGS...] are passed through to ninja
    install:    Installs the target binary
    run:        $NAME run EXECUTABLE [ARGS...]
                    Runs the EXECUTABLE on the build host, e.g.
                    'shell' or 'js', [ARGS...] are passed through to the executable
    gdb:        Same as run, but also starts a gdb remote session.
                $NAME gdb EXECUTABLE [-ex 'any gdb command']...
                    Passes through '-ex' commands to gdb
    vcpkg:      Ensure that dependencies are available
    test:       $NAME test [TEST_NAME_PATTERN]
                    Runs the unit tests on the build host, or if TEST_NAME_PATTERN
                    is specified tests matching it.
    delete:     Removes the build environment
    rebuild:    Deletes and re-creates the build environment, and compiles the project
    addr2line:  $NAME addr2line BINARY_FILE ADDRESS
                    Resolves the ADDRESS in BINARY_FILE to a file:line. It will
                    attempt to find the BINARY_FILE in the appropriate build directory

  Examples:
    $NAME run ladybird
        Runs the Ladybird browser
    $NAME run js -A
        Runs the js(1) REPL
    $NAME test
        Runs the unit tests on the build host
    $NAME addr2line RequestServer 0x12345678
        Resolves the address 0x12345678 in the RequestServer binary"""

    parser = argparse.ArgumentParser(
        description="Ladybird Build & Run script",
        epilog=epilog,
        formatter_class=argparse.RawTextHelpFormatter,
    )

    command_choices = [
        "build",
        "install",
        "run",
        "gdb",
        "test",
        "rebuild",
        "recreate",
        "addr2line",
        "delete",
        "vcpkg"
    ]
    parser.add_argument(
        "command",
        type=str,
        choices=command_choices,
        help="Supported commands:\n\t " + ", ".join(command_choices),
        metavar="command",
    )

    target_choices = ["ladybird", "js", "test-js", "test262-runner"]
    parser.add_argument(
        "target",
        type=str,
        choices=target_choices,
        help="Supported targets: " + ", ".join(target_choices),
        metavar="target",
        default=os.environ.get("SERENITY_ARCH") or "x86_64",
        nargs="?",
    )

    parser.add_argument(
        "args", type=str, nargs="*", help="The arguments to pass to the command"
    )
    

    args = parser.parse_args()
    Serenity(args).run()


if __name__ == "__main__":
    main()
