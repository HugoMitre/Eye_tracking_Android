#!/bin/bash

set -e

[ -n ${ANDROID_SDK_ROOT} ] && unset ANDROID_SDK_ROOT
[ -n ${ANDROID_SDK_ROOT} ] && unset ANDROID_HOME
echo $ANDROID_SDK_ROOT

IOS_IDENTITY="iPhone Developer: David Hirvonen (7YLQXWH6FK)"

CMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.11.sdk

HUNTER_CONFIGURATION_TYPES=Release 
BUILD_PUBLIC_SDK=ON 
BUILD_DRISHTI_ACF=OFF
BUILD_DRISHTI_FACE=OFF
BUILD_DRISHTI_MOTION=OFF
BUILD_DRISHTI_GRAPHICS=OFF
BUILD_EXAMPLES=ON
BUILD_UTILITIES=OFF
BUILD_REGRESSION_FIXED_POINT=OFF
BUILD_REGRESSION_SIMD=OFF

CPACK_TYPE=TGZ

DRISHTI_BUILD_ARGS_BASE=\
(
    "BUILD_PUBLIC_SDK=${BUILD_PUBLIC_SDK}"
    "BUILD_EXAMPLES=${BUILD_EXAMPLES}"
    "BUILD_UTILITIES=${BUILD_UTILITIES}"
    "BUILD_DRISHTI_FACE=${BUILD_DRISHTI_FACE}"
    "BUILD_DRISHTI_MOTION=${BUILD_DRISHTI_MOTION}"
    "BUILD_DRISHTI_ACF=${BUILD_DRISHTI_ACF}"
    "BUILD_XGBOOST=${BUILD_XGBOOST}"
    "BUILD_REGRESSION_FIXED_POINT=${BUILD_REGRESSION_FIXED_POINT}"
    "BUILD_REGRESSION_SIMD=${BUILD_REGRESSION_SIMD}"
)

DRISHTI_BUILD_ARGS=( "HUNTER_CONFIGURATION_TYPES=Release" "${DRISHTI_BUILD_ARGS_BASE[@]}")
DRISHTI_BUILD_ARGS_DEBUG=( "HUNTER_CONFIGURATION_TYPES=Debug" "${DRISHTI_BUILD_ARGS_BASE[@]}")

DRISHTHI_BUILD_HIDE=\
(
    "CMAKE_VISIBILITY_INLINES_HIDDEN=ON"
    "CMAKE_CXX_VISIBILITY_PRESET=hidden"
    "CMAKE_XCODE_ATTRIBUTE_GCC_INLINES_ARE_PRIVATE_EXTERN=YES"
    "CMAKE_XCODE_ATTRIBUTE_GCC_SYMBOLS_PRIVATE_EXTERN=YES"
)

function rename_tab
{
	echo -ne "\033]0;$1:$2\007"
}