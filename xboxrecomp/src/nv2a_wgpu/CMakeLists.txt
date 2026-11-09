# WebGPU renderer for the pushbuffer executor (browser only; ADR 0011,
# docs/webgpu-plan.md). Picked at run time with RECOMP_GPU=webgpu; its JS
# side (web/wgpu.js, web/wgpu_fixed.js) is linked as --js-library by the
# application.
if(NOT EMSCRIPTEN)
    return()
endif()

add_library(xbox_nv2a_wgpu STATIC
    wgpu_queue.c
    wgpu_surf.c
    wgpu_tex.c
    wgsl_vsh.c
    wgsl_psh.c
    wgpu_draw.c
    wgpu_pipe.c
    wgpu_stats.c
)
target_include_directories(xbox_nv2a_wgpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# nv2a_gl: the fallback when the render thread cannot start (and nv2a_gl.h);
# SDL (the canvas's window and input) comes with it.
target_link_libraries(xbox_nv2a_wgpu PUBLIC xbox_nv2a_gl)
target_compile_options(xbox_nv2a_wgpu PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
