# System-Name für das Ziel-OS
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Die MinGW-Compiler für Windows definieren
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_EXECUTABLE_SUFFIX ".exe")
# Wo nach Bibliotheken und Headern gesucht werden soll
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# Verhindern, dass Programme beim Testen auf dem Host-System (Linux) ausgeführt werden
set(CMAKE_CROSSCOMPILING_EMULATOR wine64) # Optional: Falls du Tests via Wine ausführen willst

# Suchverhalten für Header/Libs anpassen (Such im Ziel-Pfad, nicht auf Linux)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Workaround für MinGW ld Bug bzgl. C++20 Modulen & ODR Duplicate Symbols
add_link_options(-Wl,--allow-multiple-definition)
