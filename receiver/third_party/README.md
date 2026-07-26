# third_party

## libjpeg-turbo 1.5.3 (static, ARM NEON)

Prebuilt for the TouchPad. The PDK's own libjpeg 6.2 has no SIMD and
decodes 1024×768 at only ~10 fps; turbo's NEON path is the difference
between that and the ~20+ fps target.

Rebuild recipe (on the Linux VM):

```sh
curl -L -O https://downloads.sourceforge.net/project/libjpeg-turbo/1.5.3/libjpeg-turbo-1.5.3.tar.gz
tar xzf libjpeg-turbo-1.5.3.tar.gz && cd libjpeg-turbo-1.5.3
export PATH=$HOME/linaro-toolchain/bin:$PATH
./configure --host=arm-linux-gnueabi CC=arm-linux-gnueabi-gcc \
  CFLAGS="-O3 -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp" \
  --enable-static --disable-shared --prefix=$PWD/dist
make -j4 && make install
cp dist/lib/libjpeg.a <here>/libjpeg-turbo/
cp -r dist/include <here>/libjpeg-turbo/
```

Verify NEON made it in: `arm-linux-gnueabi-nm libjpeg.a | grep -c jsimd_.*neon`
(should be > 0).
