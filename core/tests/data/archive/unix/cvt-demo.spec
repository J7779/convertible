Name: cvt-demo
Version: 1.0
Release: 1
Summary: synthetic test package
License: MIT
BuildArch: noarch

%description
Synthetic test package.

%install
mkdir -p %{buildroot}/usr/share/doc/cvt-demo %{buildroot}/usr/bin
printf 'Hello, archive!\n' > %{buildroot}/usr/share/doc/cvt-demo/hello.txt
cat %{lorem} > %{buildroot}/usr/share/doc/cvt-demo/lorem.txt
printf '#!/bin/sh\necho hello\n' > %{buildroot}/usr/bin/cvt-demo
chmod 0755 %{buildroot}/usr/bin/cvt-demo
ln -s hello.txt %{buildroot}/usr/share/doc/cvt-demo/link
find %{buildroot} -exec touch -h -d @1623758400 {} +

%files
%dir /usr/share/doc/cvt-demo
/usr/share/doc/cvt-demo/hello.txt
/usr/share/doc/cvt-demo/lorem.txt
/usr/share/doc/cvt-demo/link
/usr/bin/cvt-demo
