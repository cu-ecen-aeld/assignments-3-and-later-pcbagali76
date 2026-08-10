#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-none-linux-gnu-}
JOBS=${JOBS:-$(nproc)}

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
	if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
		CROSS_COMPILE=aarch64-linux-gnu-
	else
		echo "Unable to find ${CROSS_COMPILE}gcc or aarch64-linux-gnu-gcc"
		exit 1
	fi
fi

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p "${OUTDIR}" || { echo "Error: Could not create output directory ${OUTDIR}"; exit 1; }
OUTDIR=$(realpath "${OUTDIR}")
ROOTFS="${OUTDIR}/rootfs"

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone "${KERNEL_REPO}" --depth 1 --single-branch --branch "${KERNEL_VERSION}" linux-stable
fi
if [ ! -e "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j"${JOBS}" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image
fi

echo "Adding the Image in outdir"
cp "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${ROOTFS}" ]
then
	echo "Deleting rootfs directory at ${ROOTFS} and starting over"
    sudo rm  -rf "${ROOTFS}"
fi

# Create necessary base directories
mkdir -p "${ROOTFS}"
cd "${ROOTFS}"
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
	git clone https://git.busybox.net/busybox
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
else
    cd busybox
fi

# Make and install busybox
make -j"${JOBS}" ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX="${ROOTFS}" install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a "${ROOTFS}/bin/busybox" | grep "program interpreter" || true
${CROSS_COMPILE}readelf -a "${ROOTFS}/bin/busybox" | grep "Shared library" || true

# Add library dependencies to rootfs
SYSROOT=$("${CROSS_COMPILE}gcc" -print-sysroot)
if [ -z "${SYSROOT}" ] || [ "${SYSROOT}" = "/" ]; then
	if [ -d "/usr/aarch64-linux-gnu" ]; then
		SYSROOT="/usr/aarch64-linux-gnu"
	else
		SYSROOT="/"
	fi
fi

INTERPRETER=$("${CROSS_COMPILE}readelf" -a "${ROOTFS}/bin/busybox" | grep "program interpreter" | sed -n 's/.*program interpreter: \(.*\)]/\1/p')
if [ -n "${INTERPRETER}" ]; then
	INTERP_PATH=$(find "${SYSROOT}" -name "$(basename "${INTERPRETER}")" -print -quit)
	if [ -n "${INTERP_PATH}" ]; then
		mkdir -p "${ROOTFS}/lib" "${ROOTFS}/lib64"
		cp -a "${INTERP_PATH}" "${ROOTFS}/lib/"
		cp -a "${INTERP_PATH}" "${ROOTFS}/lib64/" 2>/dev/null || true
	fi
fi

LIBS=$("${CROSS_COMPILE}readelf" -a "${ROOTFS}/bin/busybox" | grep "Shared library" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
for lib in ${LIBS}; do
	LIB_PATH=$(find "${SYSROOT}" -name "${lib}" -print -quit)
	if [ -n "${LIB_PATH}" ]; then
		mkdir -p "${ROOTFS}/lib" "${ROOTFS}/lib64"
		cp -a "${LIB_PATH}" "${ROOTFS}/lib/"
		cp -a "${LIB_PATH}" "${ROOTFS}/lib64/" 2>/dev/null || true
	fi
done

# Make device nodes
sudo rm -f "${ROOTFS}/dev/null" "${ROOTFS}/dev/console"
sudo mknod -m 666 "${ROOTFS}/dev/null" c 1 3
sudo mknod -m 600 "${ROOTFS}/dev/console" c 5 1

# Clean and build the writer utility
make -C "${FINDER_APP_DIR}" clean
make -C "${FINDER_APP_DIR}" CROSS_COMPILE=${CROSS_COMPILE}

# Copy the finder related scripts and executables to the /home directory on the target rootfs
cp "${FINDER_APP_DIR}/writer" "${ROOTFS}/home/"
cp "${FINDER_APP_DIR}/finder.sh" "${ROOTFS}/home/"
cp "${FINDER_APP_DIR}/finder-test.sh" "${ROOTFS}/home/"
cp "${FINDER_APP_DIR}/autorun-qemu.sh" "${ROOTFS}/home/"
mkdir -p "${ROOTFS}/home/conf"
cp "${FINDER_APP_DIR}/../conf/username.txt" "${ROOTFS}/home/conf/"
cp "${FINDER_APP_DIR}/../conf/assignment.txt" "${ROOTFS}/home/conf/"

# Modify finder-test.sh to reference conf/assignment.txt instead of ../conf/assignment.txt
sed -i 's#\.\./conf/#conf/#g' "${ROOTFS}/home/finder-test.sh"

chmod +x "${ROOTFS}/home/finder.sh" "${ROOTFS}/home/finder-test.sh" "${ROOTFS}/home/autorun-qemu.sh" "${ROOTFS}/home/writer"

# Chown the root directory
sudo chown -R root:root "${ROOTFS}"

# Create initramfs.cpio.gz
cd "${ROOTFS}"
find . | cpio -H newc -ov --owner root:root | gzip -f > "${OUTDIR}/initramfs.cpio.gz"
