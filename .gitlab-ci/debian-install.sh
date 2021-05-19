#!/bin/bash
# 
# Constructs the base container image used to build Weston within CI. Per the
# comment at the top of .gitlab-ci.yml, any changes in this file must bump the
# $FDO_DISTRIBUTION_TAG variable so we know the container has to be rebuilt.

set -o xtrace -o errexit

# These get temporary installed for building Linux and then force-removed.
LINUX_DEV_PKGS="
	bc
	bison
	flex
	libelf-dev
"

# These get temporary installed for building Mesa and then force-removed.
MESA_DEV_PKGS="
	bison
	flex
	gettext
	libwayland-egl-backend-dev
	libxrandr-dev
	llvm-8-dev
	python-mako
	python3-mako
"

# Needed for running the custom-built mesa
MESA_RUNTIME_PKGS="
	libllvm8
"

echo 'deb http://deb.debian.org/debian buster-backports main' >> /etc/apt/sources.list
apt-get update
apt-get -y --no-install-recommends install \
	autoconf \
	automake \
	build-essential \
	clang-8 \
	curl \
	doxygen \
	gcovr \
	git \
	lcov \
	libasound2-dev \
	libbluetooth-dev \
	libcairo2-dev \
	libcolord-dev \
	libdbus-1-dev \
	libegl1-mesa-dev \
	libevdev-dev \
	libexpat1-dev \
	libffi-dev \
	libgbm-dev \
	libgdk-pixbuf2.0-dev \
	libgles2-mesa-dev \
	libglu1-mesa-dev \
	libgstreamer1.0-dev \
	libgstreamer-plugins-base1.0-dev \
	libinput-dev \
	libjack-jackd2-dev \
	libjpeg-dev \
	libjpeg-dev \
	liblcms2-dev \
	libmtdev-dev \
	libpam0g-dev \
	libpango1.0-dev \
	libpixman-1-dev \
	libpng-dev \
	libpulse-dev \
	libsbc-dev \
	libsystemd-dev \
	libtool \
	libudev-dev \
	libva-dev \
	libvpx-dev \
	libvulkan-dev \
	libwebp-dev \
	libx11-dev \
	libx11-xcb-dev \
	libxcb1-dev \
	libxcb-composite0-dev \
	libxcb-xfixes0-dev \
	libxcb-cursor-dev \
	libxcb-xkb-dev \
	libxcursor-dev \
	libxkbcommon-dev \
	libxml2-dev \
	lld-8 \
	llvm-8 \
	mesa-common-dev \
	ninja-build \
	pkg-config \
	python3-pip \
	python3-setuptools \
	qemu-system \
	sysvinit-core \
	xwayland \
	$MESA_DEV_PKGS \
	$MESA_RUNTIME_PKGS \
	$LINUX_DEV_PKGS \

apt-get -y --no-install-recommends -t buster-backports install \
	freerdp2-dev


# Actually build our dependencies ...
./.gitlab-ci/build-deps.sh


# And remove packages which are only required for our build dependencies,
# which we don't need bloating the image whilst we build and run Weston.
apt-get -y --autoremove purge $LINUX_DEV_PKGS $MESA_DEV_PKGS
