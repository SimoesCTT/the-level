Name:           the-level
Version:        0.1.0
Release:        1%{?dist}
%global debug_package %{nil}
Summary:        Zero-copy CPU/GPU shared memory and an async 2D engine for i915

License:        MIT
URL:            https://github.com/SimoesCTT/the-level
Source0:        %{url}/releases/download/v%{version}/the-level-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  opencl-headers
BuildRequires:  OpenCL-ICD-Loader-devel

%description
The Level provides two small C libraries for zero-copy CPU/GPU shared
memory on Linux.

chipmem allocates an i915 GEM object, exports it as a DMA-BUF, and
provides one explicit coherency handoff (chipmem_sync) that brackets
CPU access with DMA_BUF_IOCTL_SYNC.

blit is a programmable asynchronous 2D engine on the same buffers,
submitting XY_SRC_COPY_BLT to the BLT ring with softpin. The GPU reads
and writes the same physical pages the CPU wrote.

Verified on Intel Skylake-U (HD Graphics 520, i915, kernel 7.2.13).
The negative control, with the sync removed, produces stale reads
42-99% of the time across 100-run batches.



%prep
%autosetup

%build
%make_build -C blit
%make_build -C chipmem

%check
make -C blit check
make -C chipmem check



%install
install -d %{buildroot}%{_includedir}/the-level
install -m 0644 chipmem/include/chipmem.h %{buildroot}%{_includedir}/the-level/
install -m 0644 blit/include/blit.h %{buildroot}%{_includedir}/the-level/

%files
%{_includedir}/the-level
%license LICENSE
%doc README.md

%changelog
* Tue Nov 10 2026 Americo Simoes <amexsimoes@gmail.com> - 0.1.0-1
- Initial package
