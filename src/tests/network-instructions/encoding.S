#include "network.inc"
.text
.option norvc
net.send a0,a1,a2
net.recv a0
net.recv.try a0
net.info a0,a1,a2
net.release a0,a1
net.wait a0,a1
net.wait.try a0,a1
