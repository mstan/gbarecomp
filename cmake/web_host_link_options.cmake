# Canonical Emscripten options for the supported browser host product.
# build_web.sh consumes the generated response file; runtime.cmake applies this
# same list for CMake consumers that link a browser-host executable.
function(gbarecomp_web_host_link_options out_var)
    set(_options
        -pthread
        -mtail-call
        -sPROXY_TO_PTHREAD
        -sPTHREAD_POOL_SIZE=4
        -sALLOW_MEMORY_GROWTH
        -sINITIAL_MEMORY=134217728
        -sSTACK_SIZE=16777216
        -sDEFAULT_PTHREAD_STACK_SIZE=16777216
        -sFORCE_FILESYSTEM
        -sEXPORTED_RUNTIME_METHODS=FS,ENV,addRunDependency,removeRunDependency
        -lidbfs.js
        -sENVIRONMENT=web,worker
        -sEXIT_RUNTIME=1
        --profiling-funcs
        --emit-symbol-map)
    set(${out_var} "${_options}" PARENT_SCOPE)
endfunction()
