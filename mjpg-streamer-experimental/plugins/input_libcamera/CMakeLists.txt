cmake_minimum_required(VERSION 3.5)
project(input_libcamera C CXX)

MJPG_STREAMER_PLUGIN_OPTION(input_libcamera "iibcamera input plugin")
MJPG_STREAMER_PLUGIN_COMPILE(input_libcamera input_libcamera.c libcamera_wrapper.cpp)

# Bookwormのシステムからlibcameraのパッケージ情報を取得
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera libcamera-base)

# 全体のヘッダーパスを通す
include_directories(${LIBCAMERA_INCLUDE_DIRS} ../../)

# libcameraの制御にはC++17以降が必須
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# プラグイン（共有ライブラリ）のビルド定義
#add_library(input_libcamera SHARED input_libcamera.c libcamera_wrapper.cpp)

# 必要なライブラリをリンク
target_link_libraries(input_libcamera 
    ${LIBCAMERA_LIBRARIES} 
    turbojpeg 
    pthread
)

# 出力先をmjpg-streamerの実行ファイルと同じ場所に設定
install(TARGETS input_libcamera DESTINATION .)


