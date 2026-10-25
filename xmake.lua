-- Spell Mastery Heat - SKSE plugin. GPL-3.0-or-later, see LICENSE.txt.
set_xmakever("3.0.0")
set_project("SpellMasteryHeat")
set_version("1.0.0")
set_license("GPL-3.0-or-later")
set_arch("x64")
set_languages("c++23")
-- the DLL carries its own Visual C++ runtime, so an older runtime on a player's PC cannot stop it loading
set_runtimes("MT")
add_rules("mode.releasedbg")
set_defaultmode("releasedbg")

-- GLOW_VR=1 builds for Skyrim VR only; anything else is the SE + AE build (same switch as Illuminated)
local vr_only = os.getenv("GLOW_VR") == "1"
set_config("skyrim_se", not vr_only)
set_config("skyrim_ae", not vr_only)
set_config("skyrim_vr", vr_only)

-- REPRODUCIBLE BUILDS: the same source gives the same DLL, byte for byte, in whatever folder it is built - no time stamp
-- (/Brepro: the compiler and the linker stamp a hash of the contents), no source folder in the code (/pathmap: this folder
-- is written as "."; MSVC takes it only with /experimental:deterministic), no build folder in the DLL (/PDBALTPATH: the .pdb
-- by its name only). Set before CommonLib is included, so its files are built the same way. MSVC only (tools = cl / link):
-- another toolchain never gets a switch it does not know. (A "cl::" prefix would make xmake drop /pathmap despite force.)
add_cxflags("/Brepro", "/experimental:deterministic", "/pathmap:" .. os.projectdir() .. "=.", { force = true, tools = "cl" })
add_shflags("/Brepro", "/PDBALTPATH:%_PDB%", { force = true, tools = "link" })

includes("lib/commonlibsse-ng")

-- DevBench's reports are JSON (MIT-licensed library; the same version CommonLib pins for its own JSON option)
add_requires("nlohmann_json v3.12.0")

target("SpellMasteryHeat", function()
    add_defines("NOMINMAX") -- windows.h min/max macros break std::min/max and numeric_limits::max
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "SpellMasteryHeat",
        author = "izzydoingit",
        description = "Arcane Overdrive: casting builds heat, heat builds power",
    })
    add_packages("nlohmann_json")
    -- the source is split by job (see the file map at the top of src/main.cpp); every .cpp in src is built
    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    add_includedirs("src")
    set_pcxxheader("src/PCH.h")
    -- every warning in OUR files is an error (his C++ standard, 2026-10-07: "/W4 /WX ... fix all warnings")
    set_warnings("allextra", "error")
end)

-- the clashes' and spell struggles' core (Crossfire's), tested without the game: xmake build test_clash && xmake run test_clash
target("test_clash", function()
    set_kind("binary")
    set_default(false)
    add_files("tests/test_clash.cpp") -- it includes src/ClashCore.cpp itself
    add_defines("_CRT_SECURE_NO_WARNINGS") -- the test reads the shipped rules file with fopen, as the sanitizer build does
    add_includedirs("src")
    set_warnings("allextra", "error")
end)

-- the whole meter, tested without the game: xmake build test_heat && xmake run test_heat
target("test_heat", function()
    set_kind("binary")
    set_default(false)
    add_files("tests/test_heat.cpp")
    add_includedirs("src")
    set_warnings("allextra", "error")
end)
