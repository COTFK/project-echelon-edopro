export TRAVIS_OS_NAME=linux
export VCPKG_ROOT=$PWD/vcpkg
export DEPENDENCIES_BASE_URL=https://github.com/edo9300/edopro-vcpkg-cache/releases/latest/download
export VCPKG_CACHE_7Z_URL=$DEPENDENCIES_BASE_URL/installed_x64-linux.7z
export BUILD_CONFIG=release