"""`aember godbolt`: flags taken from compile_commands.json, the Compiler
Explorer config written from them, the docker command and the source link."""

import base64
import io
import json
import tempfile
import unittest
from pathlib import Path

from aember.steps.godbolt import godbolt as ce

SDK = "/aember_ws/aember/build/aember-resources/{arch}/sdk/sysroots"
GXX = SDK + "/x86_64-pokysdk-linux/usr/bin/{triple}/{triple}-g++"


def entry(arch: str, triple: str, source: str, includes: list[str]) -> dict:
    gxx = GXX.format(arch=arch, triple=triple)
    args = [gxx, f"--sysroot={SDK.format(arch=arch)}/target", "-DSPDLOG_COMPILED_LIB"]
    args += [f"-I/aember_ws/aember/{i}" for i in includes]
    args += ["-isystem", f"/aember_ws/aember/build/vcpkg/{arch}/include"]
    args += ["-O2", "-g", "-pipe", "-march=armv8-a", "-O3", "-flto=10"]
    args += ["-std=c++23", "-Wall", "-Werror", "-o", "x.o", "-c", source]
    return {"directory": "/d", "command": " ".join(args), "file": source}


def database(arch: str, triple: str) -> list[dict]:
    pm = "/aember_ws/aember/libraries/process-manager/src/process-manager.cpp"
    return [
        entry(arch, triple, "/aember_ws/aember/services/netd/src/a.cpp", ["svc/inc"]),
        entry(arch, triple, pm, ["libraries/pm/include", "libraries/ipc/include"]),
    ]


class OptionsTest(unittest.TestCase):
    def setUp(self):
        db = database("aarch64-poky-linux", "aarch64-poky-linux")
        self.exe, self.options = ce.options_from(db)

    def test_compiler_is_the_sdk_gxx(self):
        self.assertTrue(self.exe.endswith("aarch64-poky-linux-g++"))

    def test_build_only_flags_are_dropped(self):
        for flag in ("-Werror", "-g", "-pipe", "-c", "-o", "x.o", "-flto=10"):
            self.assertNotIn(flag, self.options)
        self.assertFalse(any(o.endswith(".cpp") for o in self.options))

    def test_target_and_optimisation_flags_stay_in_order(self):
        for flag in ("-march=armv8-a", "-std=c++23", "-DSPDLOG_COMPILED_LIB"):
            self.assertIn(flag, self.options)
        # The build's last -O wins (-O3 after -O2): keep both, in order.
        self.assertLess(self.options.index("-O2"), self.options.index("-O3"))
        i = self.options.index("-isystem")
        self.assertTrue(self.options[i + 1].endswith("/include"))

    def test_includes_are_the_union_over_all_entries(self):
        inc = [o for o in self.options if o.startswith("-I")]
        self.assertEqual(
            inc,
            sorted(
                [
                    "-I/aember_ws/aember/libraries/ipc/include",
                    "-I/aember_ws/aember/libraries/pm/include",
                    "-I/aember_ws/aember/svc/inc",
                ]
            ),
        )

    def test_arguments_form_is_read_too(self):
        db = database("arm-poky-linux", "arm-poky-linux-gnueabi")
        for e in db:
            e["arguments"] = e.pop("command").split()
        exe, options = ce.options_from(db)
        self.assertTrue(exe.endswith("-g++"))
        self.assertIn("-std=c++23", options)

    def test_empty_database_is_an_error(self):
        with self.assertRaises(ValueError):
            ce.options_from([])


class CompilerTest(unittest.TestCase):
    def test_ids_names_objdump_and_instruction_sets(self):
        cases = {
            "x64-poky-linux": ("aember-x64", "amd64"),
            "x64-poky-linux-musl": ("aember-x64_musl", "amd64"),
            "aarch64-poky-linux": ("aember-aarch64", "aarch64"),
            "arm-poky-linux-musl": ("aember-arm_musl", "arm32"),
            "riscv64-poky-linux": ("aember-riscv64", "riscv64"),
        }
        for arch, (cid, iset) in cases.items():
            c = ce.Compiler(arch, "/sdk/bin/t-g++", [])
            self.assertEqual((c.id, c.instruction_set), (cid, iset), arch)
            self.assertEqual(c.objdump, "/sdk/bin/t-objdump")
            self.assertEqual(c.demangler, "/sdk/bin/t-c++filt")


class FindCompilersTest(unittest.TestCase):
    def test_configured_targets_only_x64_first_glibc_before_musl(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for arch in (
                "aarch64-poky-linux-musl",
                "x64-poky-linux-musl",
                "x64-poky-linux",
            ):
                d = root / "build/configure" / arch / "Release"
                d.mkdir(parents=True)
                db = database(arch, arch)
                (d / "compile_commands.json").write_text(json.dumps(db))
            (root / "build/configure/arm-poky-linux/Debug").mkdir(parents=True)
            found = [c.arch for c in ce.find_compilers(root, "Release")]
        self.assertEqual(
            found, ["x64-poky-linux", "x64-poky-linux-musl", "aarch64-poky-linux-musl"]
        )


class PropertiesTest(unittest.TestCase):
    def setUp(self):
        self.x64 = ce.Compiler("x64-poky-linux", "/s/x-g++", ["-O3", "-DA=b c"])
        self.arm = ce.Compiler("aarch64-poky-linux", "/s/a-g++", ["-O3"])
        text = ce.properties([self.x64, self.arm])
        self.props = dict(
            line.split("=", 1) for line in text.splitlines() if "=" in line
        )

    def test_only_aember_compilers_x64_default(self):
        self.assertEqual(self.props["compilers"], "&aember")
        self.assertEqual(self.props["defaultCompiler"], "aember-x64")
        self.assertEqual(
            self.props["group.aember.compilers"], "aember-x64:aember-aarch64"
        )

    def test_cross_compilers_dont_execute(self):
        self.assertEqual(self.props["group.aember.supportsExecute"], "false")
        self.assertEqual(self.props["group.aember.supportsBinary"], "true")

    def test_per_compiler_settings(self):
        self.assertEqual(self.props["compiler.aember-x64.exe"], "/s/x-g++")
        self.assertEqual(self.props["compiler.aember-x64.objdumper"], "/s/x-objdump")
        self.assertEqual(self.props["compiler.aember-x64.demangler"], "/s/x-c++filt")
        # Options are shell-quoted: a define with a space stays one argument.
        self.assertEqual(self.props["compiler.aember-x64.options"], "-O3 '-DA=b c'")

    def test_intel_syntax_only_for_x86(self):
        self.assertEqual(self.props["compiler.aember-x64.intelAsm"], "-masm=intel")
        self.assertEqual(self.props["compiler.aember-aarch64.intelAsm"], "")
        self.assertEqual(self.props["compiler.aember-aarch64.supportsIntel"], "false")

    def test_no_compilers_is_an_error(self):
        with self.assertRaises(ValueError):
            ce.properties([])

    def test_write_config_and_timeout(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpp, site = ce.write_config(Path(tmp) / "g", [self.x64])
            self.assertIn("compiler.aember-x64.exe", cpp.read_text())
            self.assertIn(f"compileTimeoutMs={ce.COMPILE_TIMEOUT_MS}", site.read_text())


class DockerAndLinkTest(unittest.TestCase):
    def test_run_command(self):
        cmd = ce.docker_run_command("/host/aember", 10240, 1000, 1000)
        joined = " ".join(cmd)
        self.assertEqual(cmd[-1], ce.IMAGE)
        self.assertIn("-p 127.0.0.1:10240:10240", joined)  # localhost only
        self.assertIn("/host/aember:/aember_ws/aember:ro", joined)
        self.assertIn(
            "/host/aember/build/godbolt/c++.local.properties:"
            "/ce/etc/config/c++.local.properties:ro",
            joined,
        )
        self.assertIn("-u 1000:1000", joined)

    def test_image_tag_matches_the_dockerfile(self):
        dockerfile = Path(__file__).parents[2] / "tools/godbolt/Dockerfile"
        self.assertIn(ce.IMAGE.split(":")[1], dockerfile.read_text())

    def test_client_state_link_carries_source_compiler_and_libraries(self):
        state = ce.session_state("int f();", "aember-x64", "-Os", "aember_x64_libs")
        url = ce.client_state_url("http://localhost:1/", state)
        prefix = "http://localhost:1/clientstate/"
        self.assertTrue(url.startswith(prefix))
        state = json.loads(base64.urlsafe_b64decode(url[len(prefix) :]))
        session = state["sessions"][0]
        self.assertEqual(session["source"], "int f();")
        self.assertEqual(
            session["compilers"],
            [
                {
                    "id": "aember-x64",
                    "options": "-Os",
                    "libs": [{"id": "aember_x64_libs", "ver": "build"}],
                }
            ],
        )
        no_libs = ce.session_state("x", "aember-x64")["sessions"][0]["compilers"]
        self.assertNotIn("libs", no_libs[0])

    def test_server_accepts_long_links(self):
        # Regression: `aember godbolt -f` of process-manager.cpp printed a
        # 22 KB /clientstate/ link; node's 16 KiB header limit answered 431.
        joined = " ".join(ce.docker_run_command("/h", 1, 1, 1))
        self.assertIn(
            f"NODE_OPTIONS=--max-http-header-size={ce.MAX_HEADER_BYTES}", joined
        )
        self.assertGreaterEqual(ce.MAX_HEADER_BYTES, 1024 * 1024)


class _Reply(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class ShortLinkTest(unittest.TestCase):
    def test_short_link_posts_the_state_and_prints_our_host(self):
        seen = {}

        def opener(req, timeout):
            seen["url"], seen["body"] = req.full_url, json.loads(req.data)
            return _Reply(b'{"url": "http://127.0.0.1:10240/z/AbC123"}')

        state = ce.session_state("x" * 30000, "aember-x64")
        link = ce.file_link(
            "http://127.0.0.1:10240", state, opener, public="http://localhost:10240/"
        )
        self.assertEqual(link, "http://localhost:10240/z/AbC123")
        self.assertEqual(seen["url"], "http://127.0.0.1:10240/api/shortener")
        self.assertEqual(seen["body"], state)

    def test_long_link_when_the_shortener_fails(self):
        def opener(req, timeout):
            raise OSError("connection refused")

        state = ce.session_state("int f();", "aember-x64")
        link = ce.file_link("http://127.0.0.1:1", state, opener, public="http://h:1/")
        self.assertTrue(link.startswith("http://h:1/clientstate/"))


EXPORTS = {
    # An aember-libs export tree as CMake installs it (abridged).
    "utils/utilsTargets.cmake": """
add_library(aember-libs::utils::logging STATIC IMPORTED)

set_target_properties(aember-libs::utils::logging PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
  INTERFACE_LINK_LIBRARIES "spdlog::spdlog"
)

add_library(aember-libs::utils::mount INTERFACE IMPORTED)

add_library(aember-libs::utils::package STATIC IMPORTED)

set_target_properties(aember-libs::utils::package PROPERTIES
  INTERFACE_LINK_LIBRARIES "nlohmann_json::nlohmann_json;\\$<LINK_ONLY:LibLZMA::LibLZMA>"
)
""",  # noqa: E501
    "utils/utilsTargets-release.cmake": """
set_target_properties(aember-libs::utils::logging PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libutils_logging.a"
  )
set_target_properties(aember-libs::utils::package PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libutils_package.a"
  )
""",
    "mount-manager/mount-managerTargets.cmake": """
add_library(aember-libs::mount-manager STATIC IMPORTED)

set_target_properties(aember-libs::mount-manager PROPERTIES
  INTERFACE_LINK_LIBRARIES "aember-libs::utils::logging;aember-libs::utils::mount"
)
""",
    "mount-manager/mount-managerTargets-release.cmake": """
set_target_properties(aember-libs::mount-manager PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmount-manager_aember-libs.a"
  )
""",
    "root-manager/root-managerTargets.cmake": """
add_library(aember-libs::root-manager STATIC IMPORTED)

set_target_properties(aember-libs::root-manager PROPERTIES
  INTERFACE_LINK_LIBRARIES "aember-libs::mount-manager;aember-libs::utils::logging"
)
""",
    "root-manager/root-managerTargets-release.cmake": """
set_target_properties(aember-libs::root-manager PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libroot-manager_aember-libs.a"
  )
""",
}


def export_tree(root: Path, arch: str = "x64-poky-linux") -> Path:
    base = root / "build/install" / arch / "Release/lib/cmake/aember-libs"
    for rel, text in EXPORTS.items():
        (base / rel).parent.mkdir(parents=True, exist_ok=True)
        (base / rel).write_text(text)
    return base


class LibrariesTest(unittest.TestCase):
    def test_link_order_puts_each_library_before_what_it_needs(self):
        # Regression: `-l` in the options box comes before the compiled
        # source on CE's command line, so nothing resolved; the libraries
        # go through CE's Libraries mechanism, in dependency order.
        with tempfile.TemporaryDirectory() as tmp:
            links = ce.link_order(export_tree(Path(tmp)))
        aember = [n for n in links if "_" in n]
        for before, after in [
            ("root-manager_aember-libs", "mount-manager_aember-libs"),
            ("mount-manager_aember-libs", "utils_logging"),
            ("root-manager_aember-libs", "utils_logging"),
        ]:
            self.assertLess(aember.index(before), aember.index(after))
        # Third-party ones last, spdlog before fmt; header-only and
        # INTERFACE targets link nothing.
        self.assertEqual(links[len(aember) :], ["lzma", "spdlog", "fmt"])
        self.assertNotIn("nlohmann_json", " ".join(links))
        self.assertEqual(len(links), len(set(links)))

    def test_find_library_paths_are_the_containers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            export_tree(root)
            vcpkg = "build/configure/x64-poky-linux/Release/vcpkg_installed"
            (root / vcpkg / "x64-poky-linux/lib").mkdir(parents=True)
            lib = ce.find_library(root, "x64-poky-linux", "Release")
            self.assertIsNone(ce.find_library(root, "arm-poky-linux", "Release"))
        install = "/aember_ws/aember/build/install/x64-poky-linux/Release"
        self.assertEqual(lib.include, install + "/include")
        self.assertEqual(
            lib.libpath,
            [install + "/lib", f"/aember_ws/aember/{vcpkg}/x64-poky-linux/lib"],
        )

    def test_properties_give_each_compiler_its_own_libraries(self):
        lib = ce.Library("/i", ["/l1", "/l2"], ["a", "b"])
        x64 = ce.Compiler("x64-poky-linux", "/s/x-g++", [], lib)
        arm = ce.Compiler("aarch64-poky-linux", "/s/a-g++", [])
        props = dict(
            line.split("=", 1)
            for line in ce.properties([x64, arm]).splitlines()
            if "=" in line
        )
        self.assertEqual(props["libs"], "aember_x64_libs")
        self.assertEqual(
            props["compiler.aember-x64.supportsLibraries"], "aember_x64_libs"
        )
        self.assertNotIn("compiler.aember-aarch64.supportsLibraries", props)
        v = "libs.aember_x64_libs.versions.build"
        self.assertEqual(props["libs.aember_x64_libs.versions"], "build")
        self.assertEqual(props[v + ".path"], "/i")
        self.assertEqual(props[v + ".libpath"], "/l1:/l2")
        self.assertEqual(props[v + ".staticliblink"], "a:b")


if __name__ == "__main__":
    unittest.main()
