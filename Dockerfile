# Use Fedora 34 parent image matching root Dockerfile
FROM fedora:34

# Install runtime libraries
RUN dnf update -y && dnf install binutils mesa-libGL openssl-libs qt5-qtbase qt5-qtxmlpatterns qt5-qtwebsockets qt5-qtwebchannel qt5-qtsvg taglib poppler-utils pandoc tzdata gdb heaptrack libasan -y && dnf clean all

# Hacks for Qt
RUN strip --remove-section=.note.ABI-tag /usr/lib64/libQt5Core.so.5

# Timezone
ENV TZ=Europe/Ljubljana

# Set working directory
WORKDIR /srv/bin

# Copy config and PKI
COPY core/run/etc/pki /srv/etc/pki
COPY core/run/etc/config/server.conf /srv/etc/config/server.conf
COPY core/opensslcnf.config /etc/crypto-policies/back-ends/opensslcnf.config

# Copy binary (C++ Qt Server backend)
COPY core/run/bin/omd-server /srv/bin/omd-server

# Copy Web frontend
COPY web/dist /srv/www

# Ports
EXPOSE 8081 9988 9000

# Run server when container launches
CMD ["/srv/bin/omd-server", "-c", "/srv/etc/config/server.conf"]
