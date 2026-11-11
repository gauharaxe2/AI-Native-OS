################################################################################
#
# aios-llama
#
# CPU-only llama.cpp (tag b6000): llama-cli and llama-server.
# MIT license. The GGUF weights are not part of this package.
#
################################################################################

AIOS_LLAMA_VERSION = b6000
AIOS_LLAMA_SITE = https://github.com/ggml-org/llama.cpp/archive/refs/tags
AIOS_LLAMA_SOURCE = b6000.tar.gz
AIOS_LLAMA_LICENSE = MIT
AIOS_LLAMA_LICENSE_FILES = LICENSE

# armv8-a matches QEMU's cortex-a53. Native detection would probe the build
# machine and can emit instructions the guest cannot run.
AIOS_LLAMA_CONF_OPTS = \
	-DGGML_NATIVE=OFF \
	-DGGML_CPU_ARM_ARCH=armv8-a \
	-DGGML_OPENMP=OFF \
	-DGGML_BLAS=OFF \
	-DGGML_CUDA=OFF \
	-DGGML_METAL=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_RPC=OFF \
	-DGGML_CCACHE=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=OFF \
	-DLLAMA_BUILD_TOOLS=ON \
	-DLLAMA_BUILD_SERVER=ON \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_COMMON=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DMTMD_VIDEO=OFF

$(eval $(cmake-package))
