#!/bin/sh

# if error, exit
set -e

CURRENT_DIR=$(dirname "$0")
SOURCE_DIR=${CURRENT_DIR}/..

summary_output_dir_name=auto-build-output

rm -rf ${summary_output_dir_name}

# Android ================================================================

if [ -z "${ANDROID_NDK_HOME}" ]; then
  echo "[!] ANDROID_NDK_HOME is not set. Point it at your NDK root, e.g. \$ANDROID_SDK_ROOT/ndk/<version>."
  exit 1
fi

ANDROID_TOOLCHAIN=${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake
if [ ! -f "${ANDROID_TOOLCHAIN}" ]; then
  echo "[!] NDK toolchain not found at ${ANDROID_TOOLCHAIN}"
  exit 1
fi

android_library_name=libdobby.a

# build android aarch64
output_dir_name=auto-build-workspace/android-arm64-build
echo "prepare build ${output_dir_name}"

cmake -S ${SOURCE_DIR} -B ${output_dir_name} -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=${ANDROID_TOOLCHAIN} -DANDROID_ABI="arm64-v8a" -DANDROID_PLATFORM=android-21 \
  -DDOBBY_GENERATE_SHARED=OFF -DDOBBY_DEBUG=OFF -DPlugin.Android.BionicLinkerRestriction=ON
cmake --build ${output_dir_name} --parallel 4 --target dobby

mkdir -p ${summary_output_dir_name}/android/arm64
cp ${output_dir_name}/${android_library_name} ${summary_output_dir_name}/android/arm64/${android_library_name}
mkdir -p prefab/modules/dobby/libs/android.arm64-v8a
cp ${output_dir_name}/${android_library_name} "prefab/modules/dobby/libs/android.arm64-v8a/${android_library_name}"

# build android armv7
output_dir_name=auto-build-workspace/android-armv7-build
echo "prepare build ${output_dir_name}"

cmake -S ${SOURCE_DIR} -B ${output_dir_name} -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=${ANDROID_TOOLCHAIN} -DANDROID_ABI="armeabi-v7a" -DANDROID_PLATFORM=android-21 \
  -DDOBBY_GENERATE_SHARED=OFF -DDOBBY_DEBUG=OFF -DPlugin.Android.BionicLinkerRestriction=ON
cmake --build ${output_dir_name} --parallel 4 --target dobby

mkdir -p ${summary_output_dir_name}/android/armv7
cp ${output_dir_name}/${android_library_name} ${summary_output_dir_name}/android/armv7/${android_library_name}
mkdir -p prefab/modules/dobby/libs/android.armeabi-v7a
cp ${output_dir_name}/${android_library_name} "prefab/modules/dobby/libs/android.armeabi-v7a/${android_library_name}"

# build android x86
output_dir_name=auto-build-workspace/android-x86-build
echo "prepare build ${output_dir_name}"

cmake -S ${SOURCE_DIR} -B ${output_dir_name} -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=${ANDROID_TOOLCHAIN} -DANDROID_ABI="x86" -DANDROID_PLATFORM=android-21 \
  -DDOBBY_GENERATE_SHARED=OFF -DDOBBY_DEBUG=OFF -DPlugin.Android.BionicLinkerRestriction=ON
cmake --build ${output_dir_name} --parallel 4 --target dobby

mkdir -p ${summary_output_dir_name}/android/x86
cp ${output_dir_name}/${android_library_name} ${summary_output_dir_name}/android/x86/${android_library_name}
mkdir -p prefab/modules/dobby/libs/android.x86
cp ${output_dir_name}/${android_library_name} "prefab/modules/dobby/libs/android.x86/${android_library_name}"

# build android x86_64
output_dir_name=auto-build-workspace/android-x86_64-build
echo "prepare build ${output_dir_name}"

cmake -S ${SOURCE_DIR} -B ${output_dir_name} -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=${ANDROID_TOOLCHAIN} -DANDROID_ABI="x86_64" -DANDROID_PLATFORM=android-21 \
  -DDOBBY_GENERATE_SHARED=OFF -DDOBBY_DEBUG=OFF -DPlugin.Android.BionicLinkerRestriction=ON
cmake --build ${output_dir_name} --parallel 4 --target dobby

mkdir -p ${summary_output_dir_name}/android/x86_64
cp ${output_dir_name}/${android_library_name} ${summary_output_dir_name}/android/x86_64/${android_library_name}
mkdir -p prefab/modules/dobby/libs/android.x86_64
cp ${output_dir_name}/${android_library_name} "prefab/modules/dobby/libs/android.x86_64/${android_library_name}"

## zip android prefab
#mkdir -p prefab/modules/dobby/include
#cp "include/dobby.h" "prefab/modules/dobby/include/"
#cp "builtin-plugin/BionicLinkerRestriction/bionic_linker_restriction.h" "prefab/modules/dobby/include/"
#cp "builtin-plugin/SymbolResolver/dobby_symbol_resolver.h" "prefab/modules/dobby/include/"
#cp "prefab/AndroidManifest.xml" .
#zip -r ${summary_output_dir_name}/android_prefab.aar prefab AndroidManifest.xml -x prefab/AndroidManifest.xml

if [ -n "${DOBBY_BUILD_OUTPUT_NAME}" ]; then
  tar czvf ${DOBBY_BUILD_OUTPUT_NAME} ${summary_output_dir_name}
fi
