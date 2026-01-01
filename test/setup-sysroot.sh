#!/bin/sh

set -eu

echo "=== Finit Test Sysroot Setup ==="
echo "Date: $(date)"
echo "SYSROOT: $SYSROOT"
echo "top_builddir: $top_builddir"
echo "srcdir: $srcdir"
echo "================================"
echo

# shellcheck disable=SC2154
make -C "$top_builddir" DESTDIR="$SYSROOT" install

mkdir -p "$SYSROOT/sbin/"
cp "$top_builddir/test/src/serv" "$SYSROOT/sbin/"

# shellcheck disable=SC2154
FINITBIN="$(pwd)/$top_builddir/src/finit" DEST="$SYSROOT" make -f "$srcdir/lib/sysroot.mk"

# Drop plugins we don't need in test, only causes confusing FAIL in logs.
for plugin in tty.so urandom.so rtc.so modprobe.so; do
    find "$SYSROOT" -name $plugin -delete
done

# Drop system .conf files we don't need in test, same as above
# shellcheck disable=SC2043
for conf in 10-hotplug.conf; do
    find "$SYSROOT" -name $conf -delete
done

# Update dynamic linker cache for /usr/local/lib libraries
echo "Running ldconfig in sysroot: $SYSROOT"
echo "Contents of $SYSROOT/etc/ld.so.conf:"
cat "$SYSROOT/etc/ld.so.conf" || echo "Warning: ld.so.conf not found"
echo "Libraries in $SYSROOT/usr/local/lib:"
ls -la "$SYSROOT/usr/local/lib/" 2>/dev/null || echo "Warning: /usr/local/lib not found in sysroot"
ldconfig -v -r "$SYSROOT" || echo "Warning: ldconfig failed with exit code $?"
echo "Verifying ldconfig cache was created:"
ls -la "$SYSROOT/etc/ld.so.cache" || echo "Warning: ld.so.cache not created"
