# (un)define the next line to either build for the newest or all current kernels
#define buildforkernels newest
#define buildforkernels current
%define buildforkernels akmod

# Local build fallback: override with --define "kernels $(uname -r)" if needed
%{!?kernels:%global kernels %(uname -r)}

Name:                gddr7_temp-kmod
Version:             1.0
Release:             1%{?dist}.1
Summary:             Kernel module(s) to read RTX 5090 GDDR7 DQR temperature sensors

Group:               System Environment/Kernel

License:             GPL
URL:                 https://github.com/sunnyyangyangyang/gddr7-temp
Source0:             gddr7_temp.c
Source1:             Makefile

BuildRequires:       %{_bindir}/kmodtool
# Local build: ensure kernel-devel for the running kernel is available
BuildRequires:       kernel-devel-%{_target_cpu} = %(uname -r)

%{!?kernels:BuildRequires: buildsys-build-rpmfusion-kerneldevpkgs-%{?buildforkernels:%{buildforkernels}}%{!?buildforkernels:current}-%{_target_cpu} }

# kmodtool does its magic here
%{expand:%(kmodtool --target %{_target_cpu} --repo rpmfusion --kmodname %{name} %{?buildforkernels:--%{buildforkernels}} %{?kernels:--for-kernels "%{?kernels}"} 2>/dev/null) }

%description
gddr7_temp is a kernel module that reads the RTX 5090 (GB202) GDDR7 DQR
temperature sensors directly via ioremap and exposes the hotspot /
per-module readout through /proc/gddr7_temp.

This module is reverse-engineered and unofficial. It performs read-only
access to GPU MMIO registers that are not privilege-locked, without
defeating any hardware protection.

%prep
# error out if there was something wrong with kmodtool
%{?kmodtool_check}

# print kmodtool output for debugging purposes:
kmodtool --target %{_target_cpu} --repo rpmfusion --kmodname %{name} %{?buildforkernels:--%{buildforkernels}} %{?kernels:--for-kernels "%{?kernels}"} 2>/dev/null

mkdir -p gddr7_temp-kmod-1.0
cp -a %{SOURCE0} %{SOURCE1} gddr7_temp-kmod-1.0/

for kernel_version in %{?kernel_versions}; do
    cp -a gddr7_temp-kmod-1.0 _kmod_build_${kernel_version%%___*}
done

%build
for kernel_version in %{?kernel_versions}; do
    make %{?_smp_mflags} -C "${kernel_version##*___}" M=${PWD}/_kmod_build_${kernel_version%%___*} modules
done

%install
rm -rf ${RPM_BUILD_ROOT}

for kernel_version in %{?kernel_versions}; do
    install -D -m 755 _kmod_build_${kernel_version%%___*}/gddr7_temp.ko \
        ${RPM_BUILD_ROOT}%{kmodinstdir_prefix}/${kernel_version%%___*}/%{kmodinstdir_postfix}gddr7_temp.ko
done
chmod u+x ${RPM_BUILD_ROOT}/lib/modules/*/extra/*

%{?akmod_install}

%clean
rm -rf $RPM_BUILD_ROOT

%changelog
