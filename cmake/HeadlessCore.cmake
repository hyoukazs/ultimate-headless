# Núcleo headless compartilhado: mesmas fontes/flags para headless_proto
# (experimento) e ultimate-headless (produto) — um main, dois nomes de
# target, sem fork. O includente define OTC_SRC antes do include.
# Target framework real OTCv8 (commit 3d32139, MIT), SEM client/, SEM
# renderer, SEM sound; Connection/Protocol/proxy reais, proxy inativo.

option(SANITIZE "build com sanitizers de auditoria (ex.: -DSANITIZE=address)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUAJIT REQUIRED luajit)
set(HEADLESS_OPTIONS_CPP ${CMAKE_CURRENT_LIST_DIR}/../src/headless/options.cpp)
set(HEADLESS_LOGIN_CPP ${CMAKE_CURRENT_LIST_DIR}/../src/headless/login.cpp)

function(add_headless_target TARGET_NAME MAIN_CPP)
    add_executable(${TARGET_NAME}
        ${MAIN_CPP}
        ${HEADLESS_OPTIONS_CPP}
        ${HEADLESS_LOGIN_CPP}
        ${OTC_SRC}/framework/luaengine/luainterface.cpp
        ${OTC_SRC}/framework/luaengine/luaobject.cpp
        ${OTC_SRC}/framework/luaengine/luaexception.cpp
        ${OTC_SRC}/framework/luaengine/luavaluecasts.cpp
        ${OTC_SRC}/framework/luaengine/lbitlib.cpp
        ${OTC_SRC}/framework/core/eventdispatcher.cpp
        ${OTC_SRC}/framework/core/event.cpp
        ${OTC_SRC}/framework/core/scheduledevent.cpp
        ${OTC_SRC}/framework/core/clock.cpp
        ${OTC_SRC}/framework/core/timer.cpp
        ${OTC_SRC}/framework/core/logger.cpp
        ${OTC_SRC}/framework/core/resourcemanager.cpp
        ${OTC_SRC}/framework/core/filestream.cpp
        ${OTC_SRC}/framework/http/http.cpp
        ${OTC_SRC}/framework/net/connection.cpp
        ${OTC_SRC}/framework/net/inputmessage.cpp
        ${OTC_SRC}/framework/net/outputmessage.cpp
        ${OTC_SRC}/framework/net/protocol.cpp
        ${OTC_SRC}/framework/net/packet_player.cpp
        ${OTC_SRC}/framework/net/packet_recorder.cpp
        ${OTC_SRC}/framework/proxy/proxy.cpp
        ${OTC_SRC}/framework/proxy/proxy_client.cpp
        ${OTC_SRC}/framework/platform/platform.cpp
        ${OTC_SRC}/framework/platform/unixplatform.cpp
        ${OTC_SRC}/framework/stdext/string.cpp
        ${OTC_SRC}/framework/stdext/time.cpp
        ${OTC_SRC}/framework/stdext/math.cpp
        ${OTC_SRC}/framework/stdext/demangle.cpp
        ${OTC_SRC}/framework/util/stats.cpp
        ${OTC_SRC}/framework/util/crypt.cpp
        ${OTC_SRC}/framework/graphics/graph.cpp
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${OTC_SRC}
        ${LUAJIT_INCLUDE_DIRS}
    )
    # Explicitly exclude the graphical client's binary/update launcher.
    target_compile_definitions(${TARGET_NAME} PRIVATE UH_HEADLESS)

    # Sem FW_GRAPHICS / FW_SOUND / FW_NET em nenhum TU. --gc-sections
    # colapsa símbolos não chamados. -include string substitui o PCH upstream.
    target_compile_options(${TARGET_NAME} PRIVATE -ffunction-sections -fdata-sections -include string)
    target_link_options(${TARGET_NAME} PRIVATE -Wl,--gc-sections)

    # UBSan NÃO linka neste subset (typeinfo de Application/OTMLNode/UIWidget
    # fora do target); ASan é compatível. NDEBUG iguala semântica ao Release.
    if(SANITIZE)
        target_compile_options(${TARGET_NAME} PRIVATE -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${TARGET_NAME} PRIVATE -fsanitize=${SANITIZE})
        target_compile_definitions(${TARGET_NAME} PRIVATE NDEBUG)
    endif()

    target_compile_options(${TARGET_NAME} PRIVATE ${LUAJIT_CFLAGS_OTHER})
    target_link_libraries(${TARGET_NAME} PRIVATE
        ${LUAJIT_LIBRARIES}
        boost_system
        boost_filesystem
        physfs
        zip
        z
        bz2
        ssl
        crypto
        pthread
        dl
        rt
    )
endfunction()
