# WebServer-dev

用C++实现的高性能web服务器，经过webbenchh压力测试可以实现上万的QPS

## 项目构建

1.进入项目
cd WebServer-dev

2.创建build文件夹并进入
mkdir build && cd build

3.构建项目并编译
cmake .. && make

4.运行项目
./WebServer 10000

5.浏览器访问
http://192.168.56.101:10000/index.html



