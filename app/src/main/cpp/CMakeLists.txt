cmake_minimum_required(VERSION 3.22.1)
project("fm_jni")

add_library(fm_jni SHARED
        fm_jni.cpp
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(fm_jni
        ${log-lib}
        ${android-lib}
        dl
)
