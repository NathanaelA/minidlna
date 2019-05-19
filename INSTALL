# Installation

Prerequisites:
- libavformat-dev
- libjpeg
- libsqlite
- libexif
- libid3tag
- libogg
- libvorbis
- libflac
- libavcodec
- libavutil
- libogg

### Run
```sh
./autogen.sh
./configure
make
make install
```


## Specific OS
### Ubuntu / Mint
```
sudo apt-get update
sudo apt-get install build-essential automake autopoint ffmpegthumbnailer gettext -y
sudo apt-get install debhelper dh-autoreconf gcc libavutil-dev libavcodec-dev libavformat-dev libjpeg-dev libsqlite3-dev libexif-dev libid3tag0-dev libogg-dev libvorbis-dev libflac-dev -y

cd /tmp
git clone git://github.com/NathanaelA/minidlna
cd minidlna
./autogen.sh
./configure
make
make install
cp minidlna.conf /etc/
```


### Centos 7
```
yum -y install epel-release
yum install -y gcc git libexif libexif-devel libjpeg-turbo-devel libid3tag-devel libogg-devel libvorbis-devel flac-libs flac-devel yasm

# Install SQLite (if you don't have it)
cd /tmp
wget https://www.sqlite.org/2017/sqlite-autoconf-3160200.tar.gz
tar xzf sqlite-autoconf-3160200.tar.gz
cd sqlite-autoconf-3160200
./configure
make
make install

# Install ffmpeg (if you don't have it)
cd /tmp
git clone git://source.ffmpeg.org/ffmpeg.git
cd ffmpeg
./configure --enable-shared --disable-programs
make
make install

cd /tmp
git clone git://github.com/NathanaelA/minidlna
cd minidlna
./autogen.sh
./configure
make
make install
cp minidlna.conf /etc/
```