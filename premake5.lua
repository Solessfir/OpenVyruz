local Root = path.getabsolute(_MAIN_SCRIPT_DIR)
local GlfwRoot = path.join(Root, "External/glfw")
local GeneratedRoot = path.join(Root, "Intermediate/Generated/GLFW")
local VulkanSdk = os.getenv("VULKAN_SDK")
local ShaderOutputRoot = path.join(Root, "Intermediate/Generated/Shaders")
local Slangc = (os.ishost("windows") and VulkanSdk) and path.join(VulkanSdk, "Bin/slangc.exe") or "slangc"

workspace "Vyruz"
    architecture "x86_64"
    configurations { "Debug", "Development", "Shipping" }
    location(Root)
    startproject "Vyruz"
    targetdir(path.join(Root, "Binaries/%{cfg.system}/%{cfg.buildcfg}"))
    objdir(path.join(Root, "Intermediate/Build/%{cfg.system}/%{cfg.buildcfg}/%{prj.name}"))
    staticruntime "Off"
    multiprocessorcompile "On"

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/utf-8" }
        defines { "NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS" }

    filter "system:linux"
        pic "On"

    filter "configurations:Debug"
        defines { "VYRUZ_DEBUG=1" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Development"
        runtime "Release"
        symbols "On"
        optimize "Speed"

    filter "configurations:Shipping"
        runtime "Release"
        symbols "Off"
        optimize "Full"

    filter {}

project "GLFW"
    kind "StaticLib"
    language "C"
    cdialect "C99"
    location(path.join(Root, "Intermediate/ProjectFiles"))
    warnings "Off"

    includedirs { path.join(GlfwRoot, "include"), path.join(GlfwRoot, "src") }
    files {
        path.join(GlfwRoot, "include/GLFW/*.h"),
        path.join(GlfwRoot, "src/context.c"),
        path.join(GlfwRoot, "src/egl_context.c"),
        path.join(GlfwRoot, "src/init.c"),
        path.join(GlfwRoot, "src/input.c"),
        path.join(GlfwRoot, "src/monitor.c"),
        path.join(GlfwRoot, "src/null_*.c"),
        path.join(GlfwRoot, "src/osmesa_context.c"),
        path.join(GlfwRoot, "src/platform.c"),
        path.join(GlfwRoot, "src/vulkan.c"),
        path.join(GlfwRoot, "src/window.c")
    }

    filter "system:windows"
        defines { "_GLFW_WIN32" }
        files {
            path.join(GlfwRoot, "src/wgl_context.c"),
            path.join(GlfwRoot, "src/win32_*.c")
        }

    filter "system:linux"
        defines { "_DEFAULT_SOURCE", "_GLFW_X11", "_GLFW_WAYLAND" }
        includedirs { GeneratedRoot }
        files {
            path.join(GlfwRoot, "src/glx_context.c"),
            path.join(GlfwRoot, "src/linux_joystick.c"),
            path.join(GlfwRoot, "src/posix_*.c"),
            path.join(GlfwRoot, "src/wl_*.c"),
            path.join(GlfwRoot, "src/x11_*.c"),
            path.join(GlfwRoot, "src/xkb_unicode.c")
        }

        local Commands = { string.format("mkdir -p \"%s\"", GeneratedRoot) }
        for _, Protocol in ipairs(os.matchfiles(path.join(GlfwRoot, "deps/wayland/*.xml"))) do
            local Base = path.join(GeneratedRoot, path.getbasename(Protocol))
            table.insert(Commands, string.format("wayland-scanner client-header \"%s\" \"%s-client-protocol.h\"", Protocol, Base))
            table.insert(Commands, string.format("wayland-scanner private-code \"%s\" \"%s-client-protocol-code.h\"", Protocol, Base))
        end
        prebuildmessage "Generating GLFW Wayland protocols"
        prebuildcommands(Commands)

    filter {}

-- Each .slang file compiles to <Name>.vert.h / <Name>.frag.h SPIR-V word arrays that Render/Renderer.cpp includes,
-- so no shader files ship next to the exe. This is its own project because gmake orders custom build outputs only
-- before linking, not before the objects that include them.
project "Shaders"
    kind "Utility"
    location(path.join(Root, "Intermediate/ProjectFiles"))
    files { "Source/Shaders/*.slang" }

    filter "files:Source/Shaders/*.slang"
        buildmessage "Compiling shader %{file.name}"
        buildcommands {
            "{MKDIR} \"" .. ShaderOutputRoot .. "\"",
            string.format("\"%s\" \"%%{file.abspath}\" -target spirv -profile spirv_1_6 -O2 -entry vertexMain -stage vertex -source-embed-style u32 -source-embed-name %%{file.basename}Vert -o \"%s/%%{file.basename}.vert.h\"", Slangc, ShaderOutputRoot),
            string.format("\"%s\" \"%%{file.abspath}\" -target spirv -profile spirv_1_6 -O2 -entry fragmentMain -stage fragment -source-embed-style u32 -source-embed-name %%{file.basename}Frag -o \"%s/%%{file.basename}.frag.h\"", Slangc, ShaderOutputRoot)
        }
        buildoutputs { path.join(ShaderOutputRoot, "%{file.basename}.vert.h"), path.join(ShaderOutputRoot, "%{file.basename}.frag.h") }

    filter {}

project "Vyruz"
    kind "WindowedApp"
    language "C++"
    cppdialect "C++23"
    location(path.join(Root, "Intermediate/ProjectFiles"))
    debugdir(Root)

    files { "Source/**.h", "Source/**.cpp" }
    includedirs { "Source", path.join(GlfwRoot, "include"), path.join(Root, "External/miniaudio"), path.join(Root, "External/SDL_GameControllerDB"), path.join(Root, "External/stb"), ShaderOutputRoot }
    defines { "GLFW_INCLUDE_VULKAN" }
    links { "GLFW" }
    dependson { "Shaders" }

    filter "system:windows"
        entrypoint "mainCRTStartup"
        files { "Resources/Windows/Vyruz.rc" }
        includedirs { path.join(VulkanSdk or "", "Include") }
        libdirs { path.join(VulkanSdk or "", "Lib") }
        links { "vulkan-1" }

    filter "system:linux"
        links { "vulkan", "pthread", "dl", "m" }

    filter {}
