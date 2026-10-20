add_rules("mode.debug", "mode.release", "mode.releasedbg", "mode.profile")

-- set_toolchains("llvm")
set_languages("c++23")

local function add_targets(algorithm)
    local cand_files = os.files("src/" .. algorithm .. "/*.cpp")
    for _, cpp_file in ipairs(cand_files) do
        local name = path.basename(cpp_file)
        local target_name = algorithm .. "_" .. string.lower(name)
        
        if not target(target_name) then
            target(target_name)
                set_kind("binary")
                add_files(cpp_file)
                -- add_defines("PARLAY_SEQUENTIAL")
                add_includedirs("include")
                set_targetdir("$(builddir)/" .. algorithm)
                set_rundir("./")
        end
    end
end

add_targets("card")
add_targets("knap")

local function add_ilp_targets()
    local cand_files = os.files("src/ilp/*.cpp")
    for _, cpp_file in ipairs(cand_files) do
        local name = path.basename(cpp_file)
        local target_name = "ilp_" .. string.lower(name)
        
        if not target(target_name) then
            target(target_name)
                set_kind("binary")
                add_files(cpp_file)
                add_includedirs("src/include", "~/gurobi1201/linux64/include")
                add_linkdirs("~/gurobi1201/linux64/lib")
                add_links("libgurobi_c++.a", "libgurobi120.so")
                add_includedirs("include")
                set_targetdir("$(builddir)/ilp")
                set_rundir("./")
        end
    end
end

-- add_ilp_targets()

--
-- If you want to known more usage about xmake, please see https://xmake.io
--
-- ## FAQ
--
-- You can enter the project directory firstly before building project.
--
--   $ cd projectdir
--
-- 1. How to build project?
--
--   $ xmake
--
-- 2. How to configure project?
--
--   $ xmake f -p [macosx|linux|iphoneos ..] -a [x86_64|i386|arm64 ..] -m [debug|release]
--
-- 3. Where is the build output directory?
--
--   The default output directory is `./build` and you can configure the output directory.
--
--   $ xmake f -o outputdir
--   $ xmake
--
-- 4. How to run and debug target after building project?
--
--   $ xmake run [targetname]
--   $ xmake run -d [targetname]
--
-- 5. How to install target to the system directory or other output directory?
--
--   $ xmake install
--   $ xmake install -o installdir
--
-- 6. Add some frequently-used compilation flags in xmake.lua
--
-- @code
--    -- add debug and release modes
--    add_rules("mode.debug", "mode.release")
--
--    -- add macro definition
--    add_defines("NDEBUG", "_GNU_SOURCE=1")
--
--    -- set warning all as error
--    set_warnings("all", "error")
--
--    -- set language: c99, c++11
--    set_languages("c99", "c++11")
--
--    -- set optimization: none, faster, fastest, smallest
--    set_optimize("fastest")
--
--    -- add include search directories
--    add_includedirs("/usr/include", "/usr/local/include")
--
--    -- add link libraries and search directories
--    add_links("tbox")
--    add_linkdirs("/usr/local/lib", "/usr/lib")
--
--    -- add system link libraries
--    add_syslinks("z", "pthread")
--
--    -- add compilation and link flags
--    add_cxflags("-stdnolib", "-fno-strict-aliasing")
--    add_ldflags("-L/usr/local/lib", "-lpthread", {force = true})
--
-- @endcode
--

