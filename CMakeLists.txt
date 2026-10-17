cmake_minimum_required(VERSION 3.14)
project(LottoWizard VERSION 1.0 LANGUAGES CXX)

# Wymagany standard C++ (dla nlohmann/json oraz wxWidgets)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Wyszukiwanie pakietu wxWidgets
# Szukamy podstawowych komponentów (core, base) oraz wxGrid (adv)
find_package(wxWidgets REQUIRED COMPONENTS net adv core base)
include(${wxWidgets_USE_FILE})

# 2. Wyszukiwanie pakietu SQLite3
find_package(SQLite3 REQUIRED)

# 3. Wyszukiwanie pakietu libcurl
find_package(CURL REQUIRED)

find_package(nlohmann_json REQUIRED)

# 4. Definicja plików źródłowych aplikacji
set(SOURCES
    LottoWizard.cpp LottoWizard.h
)

# Dla systemu Windows: ukrywa okno konsoli i uruchamia aplikację jako czyste GUI
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES})
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
elseif(APPLE)
    add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${SOURCES})
elseif(LINUX OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(${PROJECT_NAME} ${SOURCES})
endif()

# 5. Podłączanie bibliotek (Linkowanie)
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${wxWidgets_LIBRARIES}
        CURL::libcurl
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
)

if(LINUX OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # --- WDRAŻANIE NA LINUX (Ubuntu / WSL) ---
    include(GNUInstallDirs)

    # 1. Ustal bazowy katalog installed vcpkg
    if(NOT DEFINED VCPKG_INSTALLED_DIR)
        if(DEFINED ENV{VCPKG_ROOT})
            set(_vcpkg_base "$ENV{VCPKG_ROOT}/installed")
        else()
            set(_vcpkg_base "$ENV{HOME}/vcpkg/installed")
        endif()
    else()
        set(_vcpkg_base "${VCPKG_INSTALLED_DIR}")
    endif()

    # 2. Dopisz triplet - ustal katalog lib z bibliotekami .so
    if(DEFINED VCPKG_TARGET_TRIPLET)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            if(EXISTS "${_vcpkg_base}/${VCPKG_TARGET_TRIPLET}/debug/lib")
                set(_vcpkg_lib_dir "${_vcpkg_base}/${VCPKG_TARGET_TRIPLET}/debug/lib")
            elseif(EXISTS "${_vcpkg_base}/${VCPKG_TARGET_TRIPLET}/lib")
                set(_vcpkg_lib_dir "${_vcpkg_base}/${VCPKG_TARGET_TRIPLET}/lib")
            endif()
        else()
            if(EXISTS "${_vcpkg_base}/${VCPKG_TARGET_TRIPLET}/lib")
                set(_vcpkg_lib_dir "${_vcpkg_base}/${VCPKG_TARGET_TRIPLET}/lib")
            endif()
        endif()
    endif()

    # 3. Fallback: automatyczne wykrycie podfolderu z lib
    if(NOT DEFINED _vcpkg_lib_dir OR NOT EXISTS "${_vcpkg_lib_dir}")
        file(GLOB _vcpkg_subdirs LIST_DIRECTORIES true "${_vcpkg_base}/*")
        foreach(_sd IN LISTS _vcpkg_subdirs)
            if(EXISTS "${_sd}/lib")
                set(_vcpkg_lib_dir "${_sd}/lib")
                break()
            endif()
        endforeach()
    endif()

    message(STATUS "VCPKG lib dir = ${_vcpkg_lib_dir}")

    # 4. RPATH:
    #    BUILD_WITH_INSTALL_RPATH TRUE - binarka w build i po install używa $ORIGIN
    #    POST_BUILD kopiuje .so z vcpkg obok binarki - ldd pokaże lokalną ścieżkę
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH           "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )

    # 5. Standardowa instalacja binarki
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # 6. POST_BUILD: generuj skrypt shell z ścieżkami wbudowanymi przy generate-time
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/copy_vcpkg_libs.sh"
        CONTENT
"#!/bin/sh
BIN=\"$<TARGET_FILE:${PROJECT_NAME}>\"
VCPKG_LIB_DIR=\"${_vcpkg_lib_dir}\"
DEST_DIR=\"$<TARGET_FILE_DIR:${PROJECT_NAME}>\"

echo \"--- POST_BUILD: Kopiowanie bibliotek (ldd + LD_LIBRARY_PATH) ---\"
echo \"  BIN          : \$BIN\"
echo \"  VCPKG_LIB_DIR: \$VCPKG_LIB_DIR\"
echo \"  DEST_DIR     : \$DEST_DIR\"

if [ -z \"\$VCPKG_LIB_DIR\" ] || [ ! -d \"\$VCPKG_LIB_DIR\" ]; then
    echo \"BLAD: VCPKG_LIB_DIR nie istnieje: '\$VCPKG_LIB_DIR'\"
    exit 1
fi

# Ustaw LD_LIBRARY_PATH na katalog vcpkg, aby ldd mogło rozwiązac sciezki
# (bez tego ldd zwraca 'not found' gdy RPATH binarki wskazuje na \$ORIGIN)
LDD_OUT=\$(LD_LIBRARY_PATH=\"\$VCPKG_LIB_DIR\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\" ldd \"\$BIN\" 2>/dev/null)

echo \"\$LDD_OUT\" | awk '/=>/ { print \$3 }' | while IFS= read -r lib; do
    [ -z \"\$lib\" ] && continue
    [ \"\$lib\" = \"not\" ] && continue
    [ ! -f \"\$lib\" ] && continue
    # Pomijamy biblioteki systemowe
    case \"\$lib\" in
        /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*|/usr/lib/x86_64-linux-gnu/*)
            continue ;;
    esac
    echo \"  Kopiuję: \$lib -> \$DEST_DIR/\"
    cp -u \"\$lib\" \"\$DEST_DIR/\"
done
echo \"--- Gotowe ---\"
"
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND chmod +x "${CMAKE_BINARY_DIR}/copy_vcpkg_libs.sh"
        COMMAND "${CMAKE_BINARY_DIR}/copy_vcpkg_libs.sh"
        COMMENT "Kopiowanie zależnych bibliotek obok aplikacji (ldd + LD_LIBRARY_PATH)"
        VERBATIM
    )

    # 7. install(CODE): przy cmake --install skopiuj tylko te .so których używa binarka
    install(CODE
"
set(_bin       \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${PROJECT_NAME}\")
set(_dest_dir  \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")

message(STATUS \"INSTALL: Kopiowanie bibliotek przez ldd\")
message(STATUS \"  BIN  : \${_bin}\")
message(STATUS \"  DEST : \${_dest_dir}\")

execute_process(
    COMMAND ldd \"\${_bin}\"
    OUTPUT_VARIABLE _ldd_out
    ERROR_QUIET
)

string(REPLACE \"\n\" \";\" _ldd_lines \"\${_ldd_out}\")
foreach(_line IN LISTS _ldd_lines)
    if(_line MATCHES \"=>[ \t]+([^ \t]+\\\\.so[^ \t]*)[ \t]\")
        set(_lib \"\${CMAKE_MATCH_1}\")
        if(NOT EXISTS \"\${_lib}\")
            continue()
        endif()
        # Pomijamy biblioteki systemowe
        if(_lib MATCHES \"^(/lib|/lib64|/usr/lib|/usr/lib64|/usr/lib/x86_64-linux-gnu)\")
            continue()
        endif()
        message(STATUS \"  Instaluję: \${_lib}\")
        file(COPY \"\${_lib}\" DESTINATION \"\${_dest_dir}\")
    endif()
endforeach()
"
    )

endif()

if(APPLE)
    # 1. Włączamy automatyczne kopiowanie głównych dylibów przez vcpkg do folderu Frameworks
    set(X_VCPKG_APPLOCAL_DEPS ON CACHE BOOL "Automatycznie kopiuj zależności z vcpkg")

    # 2. Definiujemy ścieżki wewnątrz paczki .app
    set(BUNDLE_DIR "$<TARGET_FILE_DIR:${PROJECT_NAME}>/../..")
    set(FRAMEWORKS_DIR "${BUNDLE_DIR}/Contents/Frameworks")
    set(PLATFORMS_DIR "${BUNDLE_DIR}/Contents/PlugIns/platforms")
    set(RESOURCES_DIR "${BUNDLE_DIR}/Contents/Resources")

    # 2. Definiujemy bazy ścieżek vcpkg (Zmień na /debug/ jeśli budujesz wersję Debug)
    set(VCPKG_BASE_DIR "/Users/lukasz/vcpkg/installed/arm64-osx-dynamic")

    # UWAGA: Jeśli w terminalu budujesz wersję DEBUG, odkomentuj poniższą linię,
    # ponieważ vcpkg trzyma biblioteki debugowania w podfolderze /debug/
    # set(VCPKG_BASE_DIR "/Users/lukasz/vcpkg/installed/arm64-osx-dynamic/debug")
    #if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    #    set(VCPKG_COCOA_PLUGIN "${VCPKG_BASE_DIR}/debug/Qt6/plugins/platforms/libqcocoa_debug.dylib")
    #else()
    #    set(VCPKG_COCOA_PLUGIN "${VCPKG_BASE_DIR}/Qt6/plugins/platforms/libqcocoa.dylib")
    #endif()
    # 4. Ręcznie wymuszamy strukturę i naprawiamy ścieżki wtyczki
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        # Tworzymy katalog na wtyczkę graficzną
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLATFORMS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOURCES_DIR}"

        # Kopiujemy wtyczkę cocoa bezpośrednio z vcpkg
        # COMMAND ${CMAKE_COMMAND} -E copy "${VCPKG_COCOA_PLUGIN}" "${PLATFORMS_DIR}/"

        COMMENT "Wymuszanie wdrażania"
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            #COMMAND ln -s "libQt6Gui_debug.6.11.2.dylib" "${FRAMEWORKS_DIR}/libQt6Gui_debug.6.dylib"
            #COMMAND ln -s "libQt6Core_debug.6.11.2.dylib" "${FRAMEWORKS_DIR}/libQt6Core_debug.6.dylib"

            # Generujemy plik konfiguracyjny qt.conf
            COMMAND ${CMAKE_COMMAND} -E echo "[Paths]" > "${RESOURCES_DIR}/qt.conf"
            COMMAND ${CMAKE_COMMAND} -E echo "Plugins = PlugIns" >> "${RESOURCES_DIR}/qt.conf"

            # Wstrzykujemy wtyczce informację, aby szukała libQt6Gui w folderze Frameworks aplikacji
            # COMMAND install_name_tool -add_rpath "@loader_path/../../Frameworks" "${PLATFORMS_DIR}/libqcocoa_debug.dylib" 2>/dev/null || true
        )
    else()
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            #COMMAND ln -s "libQt6Gui.6.11.2.dylib" "${FRAMEWORKS_DIR}/libQt6Gui.6.dylib"
            #COMMAND ln -s "libQt6Core.6.11.2.dylib" "${FRAMEWORKS_DIR}/libQt6Core.6.dylib"

            # Generujemy plik konfiguracyjny qt.conf
            COMMAND ${CMAKE_COMMAND} -E echo "[Paths]" > "${RESOURCES_DIR}/qt.conf"
            COMMAND ${CMAKE_COMMAND} -E echo "Plugins = PlugIns" >> "${RESOURCES_DIR}/qt.conf"

            #COMMAND install_name_tool -add_rpath "@loader_path/../../Frameworks" "${PLATFORMS_DIR}/libqcocoa.dylib" 2>/dev/null || true
        )
    endif()

    # 5. Ustawiamy RPATH dla samej aplikacji
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
endif()
