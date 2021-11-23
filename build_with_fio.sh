cd /root/fio-master/
make clean
make -j

cd -
rm -rf build
TYPE=rel ./build.sh
