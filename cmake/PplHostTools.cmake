# Host tools of the repository that produce C tables compiled into the game
# (tools/dataproc: species, items, trainers, moves, Pokedex...), built for the
# machine running the build, as the DS build does (tools/meson.build).
#
# Defines PPL_DATAPROC_TOOLS (target names) and puts the executables in
# ${PPL_HOSTTOOLS_DIR}.

set(_tools ${PROJECT_SOURCE_DIR}/tools)

add_library(ppl_host_yyjson STATIC ${PPL_YYJSON}/src/yyjson.c)
target_include_directories(ppl_host_yyjson PUBLIC ${PPL_YYJSON}/src)

add_library(ppl_host_dataproc STATIC
    ${_tools}/dataproc/lib/dataproc.c
    ${_tools}/dataproc/lib/json.c
    ${_tools}/nitroarc/lib/nitroarc.c
    ${_tools}/enumproc/libenum.c
    ${_tools}/enumproc/libexpr.c
    ${_tools}/dataproc/src/common/cli.c
    ${_tools}/dataproc/src/common/driver.c
    ${_tools}/dataproc/src/common/entries.c
    ${_tools}/dataproc/src/common/lookups.c
    ${_tools}/dataproc/src/common/outputs.c
    ${_tools}/dataproc/src/common/strings.c)
target_include_directories(ppl_host_dataproc PUBLIC
    ${_tools}/dataproc/lib/include
    ${_tools}/nitroarc/lib/include
    ${_tools}/enumproc
    ${_tools}/dataproc/src
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}
    ${PPL_GENERATED_DIR})
target_compile_definitions(ppl_host_dataproc PUBLIC
    REPO_INCLUDE="${PROJECT_SOURCE_DIR}/include"
    REPO_BUILD="${PPL_GENERATED_DIR}"
    TEMPLATES_DIR="${_tools}/dataproc/data"
    _GNU_SOURCE)
target_link_libraries(ppl_host_dataproc PUBLIC ppl_host_yyjson)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ppl_host_dataproc PUBLIC $<$<COMPILE_LANGUAGE:C>:-std=gnu17> -w)
endif()
if(WIN32)
    # No open_memstream() in the Windows C library.
    target_sources(ppl_host_dataproc PRIVATE ${_tools}/pc_port/hostcompat/memstream_compat.c)
    set_property(SOURCE ${_tools}/dataproc/src/common/outputs.c APPEND PROPERTY COMPILE_OPTIONS
        "-include${_tools}/pc_port/hostcompat/memstream_compat.h")
endif()

set(PPL_DATAPROC_TOOLS "")
foreach(_tool speciesproc itemproc trainerproc moveproc dexproc frontierproc npctradeproc resdatproc)
    add_executable(ppl_tool_${_tool} ${_tools}/dataproc/src/${_tool}.c)
    target_link_libraries(ppl_tool_${_tool} PRIVATE ppl_host_dataproc)
    if(NOT WIN32)
        target_link_libraries(ppl_tool_${_tool} PRIVATE m)
    endif()
    if(MINGW)
        target_link_options(ppl_tool_${_tool} PRIVATE -static)
    endif()
    set_target_properties(ppl_tool_${_tool} PROPERTIES
        OUTPUT_NAME ${_tool} RUNTIME_OUTPUT_DIRECTORY ${PPL_HOSTTOOLS_DIR})
    list(APPEND PPL_DATAPROC_TOOLS ppl_tool_${_tool})
endforeach()

# enumproc: enum names -> numbers in preprocessed assembly (make_script_bin.sh).
add_executable(ppl_tool_enumproc ${_tools}/enumproc/enumproc.c)
target_link_libraries(ppl_tool_enumproc PRIVATE ppl_host_dataproc)
if(MINGW)
    target_link_options(ppl_tool_enumproc PRIVATE -static)
endif()
set_target_properties(ppl_tool_enumproc PROPERTIES OUTPUT_NAME enumproc RUNTIME_OUTPUT_DIRECTORY ${PPL_HOSTTOOLS_DIR})
if(WIN32)
    target_compile_options(ppl_tool_enumproc PRIVATE "-include${_tools}/pc_port/hostcompat/getline_compat.h")
endif()

# msgenc: text banks (res/text/*.json) -> binary, for the English layer
# (tools/pc_port/build_lang_en.py).
enable_language(CXX)
add_executable(ppl_tool_msgenc
    ${_tools}/msgenc/msgenc.cpp ${_tools}/msgenc/Options.cpp ${_tools}/msgenc/MessagesConverter.cpp
    ${_tools}/msgenc/MessagesDecoder.cpp ${_tools}/msgenc/MessagesEncoder.cpp ${_tools}/msgenc/Gmm.cpp
    ${_tools}/msgenc/Json.cpp ${_tools}/msgenc/pugixml.cpp)
target_compile_options(ppl_tool_msgenc PRIVATE -std=c++17 -DNDEBUG -Wno-unused-parameter)
target_link_libraries(ppl_tool_msgenc PRIVATE ppl_host_dataproc)
if(MINGW)
    target_link_options(ppl_tool_msgenc PRIVATE -static)
endif()
set_target_properties(ppl_tool_msgenc PROPERTIES OUTPUT_NAME msgenc RUNTIME_OUTPUT_DIRECTORY ${PPL_HOSTTOOLS_DIR})
