FROM  devkitpro/devkita64:20251231  AS  builder

# install dependencies
RUN   apt-get  update       \
  &&  apt-get  install  -y  \
    automake                \
    build-essential         \
    fakeroot                \
    file                    \
    zstd                    \
;

# install devkitpro
RUN   useradd  nxdt-build  \
  &&  dkp-pacman  --noconfirm  -S  dkp-toolchain-vars  \
;

WORKDIR  /app/

ENTRYPOINT  [ "/app/build-downgrade.sh" ]
